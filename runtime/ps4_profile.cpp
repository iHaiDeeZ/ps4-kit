// Burst profiler (from SSBU): while a profile runs, the app's threads (each registers at its start:
// the runtime's pthread_create wrap calls ps4_profile_thread_begin; the main thread registers at
// static init) are interrupted with SIGVTALRM every 4 ms and the handler notes the interrupted
// instruction, whether it sits on or right after a `syscall` (the thread is waiting in the kernel),
// and the return addresses that led there. Signals go to all of them at once and the handlers write
// into their own thread's slot (found through a thread-local index, not a search), so a round costs
// the profiler a few microseconds a thread. A profile lasts a few seconds: started by the app (SSBU:
// when the frame rate drops, and on L3+R3), written to <data dir>/profiles/profile_<n>.txt.
//
// Compared with the 1 kHz sampler (ps4_sampler.cpp): every thread of the app, not a registered
// few; 250 rounds a second, and only for the profile's seconds; system-module samples named by
// module ("mod libkernel.sprx+0x..") with the bytes around the busiest ones, to read on the PC which
// call each is. Off by default (ps4_profile_set_enabled).
//
// Which threads: by the start of their names (ps4_profile_set_threads), all of them if none are
// given. On SMW a signal sampler froze the game after ~40 s: a thread that waits on the GPU stuck in
// a libkernel wait it interrupted. Mutex and condition variable waits take an interruption (libthr
// starts the wait again); GPU submission, display and audio threads should be left out (SSBU samples
// only its own and DXVK's threads). The sampled thread list is in the profile's header.
//
// Addresses: "elf 0x.." (relative to .text, as crash.log), "mod <module>+0x.." (system modules) or
// "abs 0x.." (code loaded at run time: JIT, NROs). scripts/profile.py names them and adds them up
// by function.

#define PS4_KIT_DIAG_INTERNAL
#include "ps4_kit_diag.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>

using namespace ps4diag;

#define PS4_WEAK extern "C" __attribute__((weak, visibility("hidden")))
PS4_WEAK const char* ps4_profile_thread_description(int tid);  // the port's full thread names
// the system's (the runtime's wrap would register the profiler's own thread to be sampled)
PS4_WEAK int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);

extern "C" int pthread_getthreadid_np(void);  // libkernel (FreeBSD libthr)
extern "C" int ps4k_scePthreadGetname(pthread_t thread, char* name) __asm__("scePthreadGetname");

namespace {

constexpr int kSigProfile = 26;  // SIGVTALRM (FreeBSD): SIGPROF is the 1 kHz sampler's
constexpr int kSaOnstack = 0x0001, kSaRestart = 0x0002, kSaSiginfo = 0x0040;
constexpr uint64_t kWaitingTag = 1ULL << 63;
constexpr int kSlots = 128, kRing = 2048, kCallers = 4;  // threads alive at once; samples a thread per profile
constexpr long kPeriodNs = 4000000;                      // a round every 4 ms

enum : uint32_t { kFree, kAlive, kEnded };  // kEnded: the thread is gone, its samples not yet written

// A slot's samples: allocated when its thread is first sampled (80 KiB; SSBU kept 10 MiB of these
// for 128 slots whether it ever profiled or not), kept for the slot's later threads.
struct Ring {
    uint64_t samples[kRing];
    uint64_t callers[kRing][kCallers];  // the eboot code that called the sampled function, innermost first
};

struct Slot {
    std::atomic<uint32_t> state{kFree};
    std::atomic<pthread_t> thread{};
    int tid = 0;
    bool checked = false, sampled = false;  // (this profile: its name looked at)
    char name[64] = {};
    uintptr_t stack_top = 0;  // (its stack, as far as a scan may read: its start's frame)
    std::atomic<Ring*> ring{nullptr};
    std::atomic<uint32_t> count{0};
};
Slot g_slots[kSlots];
std::atomic<bool> g_enabled{false}, g_running{false}, g_busy{false};  // busy: from the start until it is written
std::atomic<int> g_profiles{0};
pthread_key_t g_end_key;  // its destructor: the thread's end (also by pthread_exit)
__thread int t_slot __attribute__((tls_model("initial-exec"))) = -1;

// The threads to sample (ps4_profile_set_threads), by name prefix; none: all of them.
constexpr int kMaxPrefixes = 16;
char g_prefixes[kMaxPrefixes][32];
std::atomic<int> g_prefix_count{0};

void onProfile(int, siginfo_t*, void* context) {
    const int i = t_slot;
    if (i < 0 || !g_running.load(std::memory_order_relaxed))
        return;
    Slot& s = g_slots[i];
    Ring* ring = s.ring.load(std::memory_order_acquire);
    if (!ring)
        return;
    const uint64_t* regs = regsOf(context);
    uint64_t rip = regs[kRipIndex], callers[kCallers] = {};
    int ncallers = 0;
    if (inSyscall(rip))
        rip |= kWaitingTag;
    // The eboot code that called in: return addresses into the eboot's code on the stack, within its
    // stack (16 KiB at most; on a fiber's, whose top is not known, to the end of the 16 KiB page rsp
    // is in: a fiber that has just started is near its stack's top, and what lies past it may not be
    // mapped). For every sample: a busy eboot function's own callers too (SSBU v00.13: who reads the
    // clock so often). Found by scanning, so now and then a stale one.
    const uintptr_t rsp = regs[kRspIndex];
    const uintptr_t limit = std::min<uintptr_t>(
        rsp < s.stack_top && s.stack_top - rsp < (64u << 20) ? s.stack_top : (rsp | 0x3FFF) + 1, rsp + 16384);
    for (uintptr_t at = rsp & ~7ull; at + 8 <= limit && ncallers < kCallers; at += 8) {
        const uint64_t v = *reinterpret_cast<const uint64_t*>(at);
        if (isReturnAddress(v))
            callers[ncallers++] = v;
    }
    const uint32_t n = s.count.load(std::memory_order_relaxed);
    if (n < kRing) {
        ring->samples[n] = rip;
        for (int k = 0; k < kCallers; k++)
            ring->callers[n][k] = callers[k];
        s.count.store(n + 1, std::memory_order_release);
    }
}

void installHandler() {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    void (*handler)(int, siginfo_t*, void*) = onProfile;
    memcpy(&action, &handler, sizeof(handler));  // the handler union's member names differ
    action.sa_flags = kSaSiginfo | kSaRestart | kSaOnstack;
    sigemptyset(&action.sa_mask);
    sigaction(kSigProfile, &action, nullptr);
}

double nowSeconds() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// Loaded system modules and their segments, so their samples can be named by module offset.
struct ModuleSegment {
    void* address;
    uint32_t size;
    int32_t prot;
};
struct ModuleInfo {
    size_t size;
    char name[256];
    ModuleSegment segments[4];
    uint32_t segment_count;
    uint8_t fingerprint[20];
};
extern "C" int32_t ps4k_sceKernelGetModuleList(int32_t* handles, size_t count, size_t* available) __asm__(
    "sceKernelGetModuleList");
extern "C" int32_t ps4k_sceKernelGetModuleInfo(int32_t handle, ModuleInfo* info) __asm__("sceKernelGetModuleInfo");

struct Module {
    uint64_t start, end;
    char name[48];
};
Module g_modules[96];
int g_module_count = 0;

void listModules() {
    int32_t handles[96];
    size_t count = 0;
    g_module_count = 0;
    if (ps4k_sceKernelGetModuleList(handles, 96, &count) != 0)
        return;
    for (size_t m = 0; m < count && m < 96; m++) {
        ModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (ps4k_sceKernelGetModuleInfo(handles[m], &info) != 0 || !info.segment_count)
            continue;
        Module& out = g_modules[g_module_count++];
        out.start = reinterpret_cast<uint64_t>(info.segments[0].address);
        out.end = out.start + info.segments[0].size;  // (the first segment: its code)
        snprintf(out.name, sizeof(out.name), "%s", info.name);
    }
}

// An address as the report writes it.
void describe(uint64_t rip, char* out, size_t n) {
    if (isCode(rip)) {
        snprintf(out, n, "elf 0x%llx", static_cast<unsigned long long>(toElf(rip)));
        return;
    }
    for (int m = 0; m < g_module_count; m++) {
        if (rip >= g_modules[m].start && rip < g_modules[m].end) {
            snprintf(out, n, "mod %s+0x%llx", g_modules[m].name, static_cast<unsigned long long>(rip - g_modules[m].start));
            return;
        }
    }
    snprintf(out, n, "abs 0x%llx", static_cast<unsigned long long>(rip));
}

struct Entry {
    uint64_t rip, caller[kCallers];
    uint32_t count;
};
bool sameSample(const Entry& a, const Entry& b) { return a.rip == b.rip && !memcmp(a.caller, b.caller, sizeof(a.caller)); }
Entry g_sorted[kRing], g_entries[kRing];

// The busiest system-module addresses of the whole profile, whose code is written with it (to read on
// the PC which call each is: libkernel's are mostly system call stubs).
struct HotSystem {
    uint64_t rip;
    uint32_t count;
};
HotSystem g_hot_system[16];

void noteHotSystem(uint64_t rip, uint32_t count) {
    if (!isSystem(rip))
        return;
    int slot = -1;
    for (int k = 0; k < 16; k++) {
        if (g_hot_system[k].rip == rip) {
            g_hot_system[k].count += count;
            return;
        }
        if (slot < 0 || g_hot_system[k].count < g_hot_system[slot].count)
            slot = k;
    }
    if (count > g_hot_system[slot].count)
        g_hot_system[slot] = {rip, count};
}

void writeThread(FILE* f, Slot& s, uint32_t rounds) {
    Ring* ring = s.ring.load(std::memory_order_acquire);
    const uint32_t n = ring ? std::min<uint32_t>(s.count.load(std::memory_order_acquire), kRing) : 0;
    if (!n) {
        fprintf(f, "thread %d \"%s\": no samples\n", s.tid, s.name);
        return;
    }
    uint32_t waiting = 0;
    for (uint32_t i = 0; i < n; i++) {
        g_sorted[i].rip = ring->samples[i];
        memcpy(g_sorted[i].caller, ring->callers[i], sizeof(g_sorted[i].caller));
        g_sorted[i].count = 1;
        waiting += (g_sorted[i].rip & kWaitingTag) != 0;
    }
    std::sort(g_sorted, g_sorted + n, [](const Entry& a, const Entry& b) {
        return a.rip != b.rip ? a.rip < b.rip : memcmp(a.caller, b.caller, sizeof(a.caller)) < 0;
    });
    uint32_t unique = 0;
    for (uint32_t i = 0; i < n;) {
        uint32_t j = i;
        while (j < n && sameSample(g_sorted[j], g_sorted[i]))
            j++;
        g_entries[unique] = g_sorted[i];
        g_entries[unique++].count = j - i;
        noteHotSystem(g_sorted[i].rip & ~kWaitingTag, j - i);
        i = j;
    }
    std::sort(g_entries, g_entries + unique, [](const Entry& a, const Entry& b) { return a.count > b.count; });
    fprintf(f, "thread %d \"%s\": %u samples of %u rounds, %.1f%% busy\n", s.tid, s.name, n, rounds,
            100.0 * (n - waiting) / n);
    char where[96], from[300];
    for (uint32_t i = 0; i < unique && i < 400; i++) {
        if (g_entries[i].count < 2 && i >= 40)
            break;
        describe(g_entries[i].rip & ~kWaitingTag, where, sizeof(where));
        size_t len = 0;
        from[0] = 0;
        for (int k = 0; k < kCallers && g_entries[i].caller[k] && len + 4 < sizeof(from); k++) {
            len += snprintf(from + len, sizeof(from) - len, " < ");
            describe(g_entries[i].caller[k], from + len, sizeof(from) - len);
            len = strlen(from);
        }
        fprintf(f, "  %5u %s%s%s\n", g_entries[i].count, where, (g_entries[i].rip & kWaitingTag) ? " wait" : "", from);
    }
}

void writeHotSystemCode(FILE* f) {
    char where[96];
    for (HotSystem& h : g_hot_system) {
        if (!h.count)
            continue;
        describe(h.rip, where, sizeof(where));
        // 48 bytes before it, 16 from it, inside its 4 KiB page (system code is readable: the handler
        // reads it too)
        const uint64_t from = std::max<uint64_t>(h.rip - 48, h.rip & ~0xFFFull);
        const uint64_t to = std::min<uint64_t>(h.rip + 16, (h.rip | 0xFFFull) + 1);
        fprintf(f, "code %s (%u samples) from -0x%llx:", where, h.count, static_cast<unsigned long long>(h.rip - from));
        for (uint64_t a = from; a < to; a++)
            fprintf(f, " %02x", *reinterpret_cast<const uint8_t*>(a));
        fprintf(f, "\n");
        h = {};
    }
}

struct Run {
    double seconds;
    char reason[96];
};
Run g_run;  // (one profile at a time: g_busy)

// A live thread's name (the port's whole description, else the system's) and whether it is one to
// sample; its samples' memory when it is.
void check(Slot& s) {
    s.checked = true;
    const char* description = ps4_profile_thread_description ? ps4_profile_thread_description(s.tid) : nullptr;
    if (description && *description) {
        snprintf(s.name, sizeof(s.name), "%s", description);
    } else {
        char name[40] = {};
        ps4k_scePthreadGetname(s.thread.load(), name);
        snprintf(s.name, sizeof(s.name), "%s", name);
    }
    const int prefixes = g_prefix_count.load(std::memory_order_acquire);
    s.sampled = prefixes == 0;
    for (int p = 0; p < prefixes && !s.sampled; p++)
        s.sampled = strncmp(s.name, g_prefixes[p], strlen(g_prefixes[p])) == 0;
    if (s.sampled && !s.ring.load(std::memory_order_acquire)) {
        auto* ring = static_cast<Ring*>(malloc(sizeof(Ring)));
        Ring* expected = nullptr;
        if (!ring || !s.ring.compare_exchange_strong(expected, ring)) {
            free(ring);
            s.sampled = s.ring.load() != nullptr;
        }
    }
}

void* profileThread(void*) {
    const Run run = g_run;
    for (Slot& s : g_slots) {
        s.count.store(0);
        s.checked = s.sampled = false;
    }
    g_running.store(true);
    const double start = nowSeconds();
    uint32_t rounds = 0;
    const timespec period = {0, kPeriodNs};
    while (nowSeconds() - start < run.seconds && g_enabled.load(std::memory_order_relaxed)) {
        const bool look = rounds % 25 == 0;  // (new threads, every 100 ms)
        for (Slot& s : g_slots) {
            if (s.state.load(std::memory_order_acquire) != kAlive)
                continue;
            if (look && !s.checked)
                check(s);
            if (s.sampled)
                pthread_kill(s.thread.load(std::memory_order_relaxed), kSigProfile);
        }
        rounds++;
        nanosleep(&period, nullptr);
    }
    g_running.store(false);
    const double took = nowSeconds() - start;
    nanosleep(&period, nullptr);  // (handlers still running finish)

    const int number = g_profiles.fetch_add(1) + 1;
    char path[160];
    snprintf(path, sizeof(path), "%s/profiles", ps4_diag_data_dir());
    mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/profiles/profile_%d.txt", ps4_diag_data_dir(), number);
    FILE* f = fopen(path, "w");
    if (f) {
        listModules();
        fprintf(f, "profile %d: %s; %.1f s, %u rounds (every %ld ms); text at %p\n", number, run.reason, took, rounds,
                kPeriodNs / 1000000, static_cast<void*>(__text_start));
        const int prefixes = g_prefix_count.load();
        fprintf(f, "threads sampled: %s", prefixes ? "" : "all");
        for (int p = 0; p < prefixes; p++)
            fprintf(f, "%s\"%s\"", p ? ", " : "", g_prefixes[p]);
        fprintf(f, "\n");
        for (int m = 0; m < g_module_count; m++)
            fprintf(f, "module %s 0x%llx-0x%llx\n", g_modules[m].name, static_cast<unsigned long long>(g_modules[m].start),
                    static_cast<unsigned long long>(g_modules[m].end));
        uint32_t left_alone = 0;
        for (Slot& s : g_slots) {
            if (s.state.load() == kFree || !s.checked)
                continue;
            if (s.sampled)
                writeThread(f, s, rounds);
            else
                fprintf(f, "%s thread %d \"%s\"\n", left_alone++ ? "     " : "not sampled:", s.tid, s.name);
        }
        writeHotSystemCode(f);
        fclose(f);
    }
    for (Slot& s : g_slots) {
        uint32_t ended = kEnded;
        s.state.compare_exchange_strong(ended, kFree);
        s.count.store(0);
    }
    ps4_boot_tracef("profile %d (%s): %.1f s sampled, %s", number, run.reason, took, f ? path : "could not be written");
    g_busy.store(false);
    return nullptr;
}

void threadEnded(void* value) {
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(value)) - 1;
    t_slot = -1;
    if (i < 0 || i >= kSlots)
        return;
    Slot& s = g_slots[i];
    // during a profile: its samples are written with it
    s.state.store(g_busy.load() && s.checked ? kEnded : kFree);
}

}  // namespace

// Each thread, at its start (the runtime's pthread_create wrap; the main thread: below).
extern "C" void ps4_profile_thread_begin() {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, [] {
        pthread_key_create(&g_end_key, threadEnded);
        installHandler();
    });
    if (t_slot >= 0)
        return;
    for (int i = 0; i < kSlots; i++) {
        uint32_t expected = kFree;
        if (g_slots[i].state.load(std::memory_order_relaxed) == kFree &&
            g_slots[i].state.compare_exchange_strong(expected, kAlive)) {
            g_slots[i].count.store(0);
            g_slots[i].name[0] = 0;
            g_slots[i].checked = g_slots[i].sampled = false;
            g_slots[i].stack_top = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
            g_slots[i].tid = pthread_getthreadid_np();
            g_slots[i].thread.store(pthread_self());
            t_slot = i;
            pthread_setspecific(g_end_key, reinterpret_cast<void*>(static_cast<intptr_t>(i) + 1));
            return;
        }
    }
}

namespace {
// The main thread, which no pthread_create wrap sees (SSBU called this from its main()).
__attribute__((constructor(102))) void registerMainThread() { ps4_profile_thread_begin(); }
}  // namespace

extern "C" void ps4_profile_set_enabled(int on) { g_enabled.store(on != 0); }
extern "C" int ps4_profile_enabled() { return g_enabled.load() ? 1 : 0; }
extern "C" int ps4_profile_running() { return g_busy.load() ? 1 : 0; }

extern "C" void ps4_profile_set_threads(const char* const* prefixes, int count) {
    if (g_busy.load())
        return;  // (not while a profile looks at the names)
    g_prefix_count.store(0, std::memory_order_release);
    int n = 0;
    for (int i = 0; prefixes && i < count && n < kMaxPrefixes; i++) {
        if (prefixes[i] && prefixes[i][0])
            snprintf(g_prefixes[n++], sizeof(g_prefixes[0]), "%s", prefixes[i]);
    }
    g_prefix_count.store(n, std::memory_order_release);
}

// A profile of `seconds`, in a thread of its own; false while one runs, or while profiling is off.
extern "C" bool ps4_profile_start(double seconds, const char* reason) {
    if (!g_enabled.load() || g_busy.exchange(true))
        return false;
    g_run.seconds = seconds > 0 ? seconds : 3.0;
    snprintf(g_run.reason, sizeof(g_run.reason), "%s", reason ? reason : "?");
    pthread_t thread;
    const bool ok = (__real_pthread_create ? __real_pthread_create(&thread, nullptr, profileThread, nullptr)
                                           : pthread_create(&thread, nullptr, profileThread, nullptr)) == 0;
    if (ok)
        pthread_detach(thread);
    else
        g_busy.store(false);
    return ok;
}
