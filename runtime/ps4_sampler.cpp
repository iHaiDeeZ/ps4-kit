// Sampling profiler, thread dumps and CPU-busy report for PS4 builds.
//
// PS4 has no perf/Razor access for homebrew, so the app samples itself:
//
// - The 1 kHz sampler (ps4_sampler_set_enabled, off by default): a helper thread sends SIGPROF to
//   each registered thread (ps4_sampler_register_thread) every millisecond, and the handler records
//   the interrupted instruction pointer. Every 10 seconds the samples are aggregated and appended to
//   <data dir>/samples.log: per thread, the busiest eboot addresses ("elf", relative to .text,
//   symbolize with `llvm-symbolizer --obj=<elf> 0x...`), system module addresses ("sys", mapped by
//   the module list and libkernel's entry points at the top of the file) and JIT code addresses
//   ("jit", 16-byte buckets; with the guest function when the port has ps4_jit_owner); then for
//   the samples inside system libraries, the eboot return addresses that led there ("wait" lines:
//   who waits, and for what). Every 60 s the busiest JIT code (64 KiB chunks) is copied to
//   jitcode.bin, the guest RAM given to ps4_sampler_set_guest_ram to mem1.bin and the busiest guest
//   functions to hotfuncs.bin, so the generated x86 and the guest code behind it can be
//   disassembled offline (scripts/profile.py, scripts/hotfuncs.py).
// - Thread dumps (ps4_watch_thread, ps4_dump_thread_stacks): every thread of the app registers
//   (the runtime's pthread_create wrap); a dump signals each one and records its instruction
//   pointer, its return addresses and the lock it waits on. For the app's watchdog and the crash
//   handler.
// - The hot-spot profiler (ps4_hot_profiler_set_enabled, off by default): the thread dump's signal
//   at a low rate, mostly to threads that are running - for long sessions.
// - The CPU-busy report (ps4_thread_busy_report) for the app's on-screen monitor.
//
// Reading a profile on the PS4:
// - A thread that looks stuck in one place may be spinning: dlmalloc's spin locks (USE_SPIN_LOCKS)
//   showed threads "frozen" in mspace_malloc that were livelocked on the heap lock - fixed in the
//   ports by blocking locks, not by the profiler. Signals make such a livelock more likely: each
//   interruption lengthens the time a lock is held.
// - libkernel 0x800003710 at the top of a thread = a restarted system call (its _umtx_op stub at
//   +0x371a): a signal interrupted a wait, SA_RESTART put the thread back on the `syscall`. It is
//   waiting, not working ("wait" lines and the hot-spot profiler tell the two apart).
// - The 1 kHz rate interrupts waits a thousand times a second; SMW froze after ~40 s with its
//   replay thread stuck in an interrupted libkernel wait. Register only CPU-bound threads for it,
//   and prefer the hot-spot or burst profilers (ps4_profile.cpp) for threads that wait on the GPU.
//
// Merged from the ports' copies (DolphinPS4 lineage); docs/merge-notes/diagnostics.md says which
// fix came from where.

#define PS4_KIT_DIAG_INTERNAL
#include "ps4_kit_diag.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>

using namespace ps4diag;

#define PS4_WEAK extern "C" __attribute__((weak, visibility("hidden")))
PS4_WEAK int ps4_jit_owner(uint64_t rip, uint32_t* guest_start, uint64_t* host_start);
PS4_WEAK void ps4_jit_dump_function(int fd, uint32_t guest_start, uint64_t host_start, uint32_t samples);
PS4_WEAK void ps4_crash_jit_info(int fd, uint64_t rip, int brief);
PS4_WEAK void ps4_recompiler_status(int fd);   // the emulator's (Crema)
PS4_WEAK void ps4_hang_guest_threads(int fd);  // the emulator's (Crema)
PS4_WEAK void ps4_heap_private_arena(const char* name);  // the runtime's

extern "C" int pthread_getthreadid_np(void);  // libkernel (FreeBSD libthr)
// Declared by hand under other names: the toolchain headers give some of these other prototypes
// (SRB2's build failed on the clash).
extern "C" int32_t ps4k_sceKernelQueryMemoryProtection(void* address, void** start, void** end,
                                                       uint32_t* protection) __asm__("sceKernelQueryMemoryProtection");
extern "C" int32_t ps4k_sceKernelGetModuleList(int32_t* handles, size_t count, size_t* available) __asm__(
    "sceKernelGetModuleList");
extern "C" int32_t ps4k_sceKernelDlsym(int32_t handle, const char* symbol, void** address) __asm__("sceKernelDlsym");

// Emulated guest RAM, dumped with the JIT code (Dolphin's Memmap.cpp sets these directly).
extern "C" {
void* ps4_profile_guest_ram = nullptr;
uint32_t ps4_profile_guest_ram_size = 0;
}

namespace {

constexpr int kSigProf = 27;   // FreeBSD values
constexpr int kSigDump = 30;   // SIGUSR1
constexpr int kSigCpu = 31;    // SIGUSR2
constexpr int kSaOnstack = 0x0001, kSaRestart = 0x0002, kSaSiginfo = 0x0040;
// Tagged sample values: bit 63 JIT code (anything outside the eboot and the system modules), bit 62
// a system module address (kept exact: the module list maps it).
constexpr uint64_t kJitTag = 1ULL << 63, kSystemTag = 1ULL << 62;
constexpr uintptr_t kPage = 16 * 1024;  // a PS4 page: mapping granularity

void installHandler(int sig, void (*handler)(int, siginfo_t*, void*)) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    memcpy(&action, &handler, sizeof(handler));  // the handler union's member names differ
    // interrupted waits resume; on the thread's signal stack (ps4_install_signal_stack)
    action.sa_flags = kSaSiginfo | kSaRestart | kSaOnstack;
    sigemptyset(&action.sa_mask);
    sigaction(sig, &action, nullptr);
}

void writeLine(int fd, const char* text) {
    size_t length = strlen(text);
    while (length > 0) {
        const ssize_t written = write(fd, text, length);
        if (written <= 0)
            return;
        text += written;
        length -= static_cast<size_t>(written);
    }
}

// a new file in the app's folder
int createDataFile(const char* name) {
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", ps4_diag_data_dir(), name);
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
}

double now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

bool isReadable(uint64_t address, uint64_t size) {
    void* start = nullptr;
    void* end = nullptr;
    uint32_t protection = 0;
    return address >= 0x10000 &&
           ps4k_sceKernelQueryMemoryProtection(reinterpret_cast<void*>(address), &start, &end, &protection) == 0 &&
           (protection & 1) && address + size <= reinterpret_cast<uintptr_t>(end);
}

// ---------------------------------------------------------------------------------------------
// Loaded modules and their segments, so "sys" samples can be mapped to module offsets.
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
extern "C" int32_t ps4k_sceKernelGetModuleInfo(int32_t handle, ModuleInfo* info) __asm__("sceKernelGetModuleInfo");

// Sony's libraries have no symbols: name the kernel library's entry points where time goes
// (scripts/profile.py maps a sample to the nearest one below it). Crema.
void writeKernelSymbols(int fd, int32_t handle) {
    static const char* const kNames[] = {
        "sched_yield", "nanosleep", "usleep", "sceKernelUsleep", "sceKernelNanosleep", "clock_gettime",
        "gettimeofday", "sceKernelGettimeofday", "sceKernelGetProcessTime", "sceKernelReadTsc",
        "sceKernelGetTscFrequency", "pthread_mutex_lock", "pthread_mutex_unlock", "pthread_mutex_trylock",
        "pthread_mutex_timedlock", "pthread_cond_wait", "pthread_cond_timedwait", "pthread_cond_signal",
        "pthread_cond_broadcast", "pthread_rwlock_rdlock", "pthread_rwlock_wrlock", "pthread_rwlock_unlock",
        "pthread_rwlock_tryrdlock", "scePthreadMutexLock", "scePthreadMutexUnlock", "scePthreadMutexTrylock",
        "scePthreadCondWait", "scePthreadCondTimedwait", "scePthreadCondSignal", "scePthreadCondBroadcast",
        "scePthreadYield", "pthread_yield", "pthread_self", "pthread_getspecific", "pthread_setspecific",
        "pthread_kill", "pthread_sigmask", "sigprocmask", "sigaction", "sigsuspend", "sigwait", "_umtx_op",
        "sem_wait", "sem_post", "sem_timedwait", "sceKernelWaitSema", "sceKernelSignalSema",
        "sceKernelWaitEventFlag", "sceKernelSetEventFlag", "sceKernelWaitEqueue", "sceKernelTriggerUserEvent",
        "kevent", "poll", "select", "read", "write", "pread", "pwrite", "open", "close", "ioctl", "fstat",
        "mmap", "munmap", "mprotect", "sceKernelMprotect", "madvise", "getpid", "getrusage",
        "sceKernelGetCurrentCpu", "__tls_get_addr", "__error", "sceKernelMapNamedFlexibleMemory",
        "sceKernelVirtualQuery", "sceKernelSendNotificationRequest", "sceKernelGetCpumode",
        "sceKernelIsNeoMode", "pthread_create", "pthread_join", "pthread_exit", "sceKernelDebugOutText"};
    char line[160];
    for (const char* name : kNames) {
        void* address = nullptr;
        if (ps4k_sceKernelDlsym(handle, name, &address) == 0 && address) {
            snprintf(line, sizeof(line), "symbol %s %p\n", name, address);
            writeLine(fd, line);
        }
    }
}

void writeModuleList(int fd, size_t* listed) {
    int32_t handles[256];
    size_t count = 0;
    // relisted when modules get loaded (e.g. RADV's GnmDriver)
    if (ps4k_sceKernelGetModuleList(handles, 256, &count) != 0 || count == *listed)
        return;
    *listed = count;
    char line[512];
    for (size_t m = 0; m < count && m < 256; m++) {
        ModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (ps4k_sceKernelGetModuleInfo(handles[m], &info) != 0)
            continue;
        int len = snprintf(line, sizeof(line), "module %s", info.name);
        for (uint32_t g = 0; g < info.segment_count && g < 4; g++)
            len += snprintf(line + len, sizeof(line) - len, " seg %p+0x%x/%d", info.segments[g].address,
                            info.segments[g].size, info.segments[g].prot);
        snprintf(line + len, sizeof(line) - len, "\n");
        writeLine(fd, line);
        if (strstr(info.name, "libkernel"))
            writeKernelSymbols(fd, handles[m]);
    }
}

// ---------------------------------------------------------------------------------------------
// The 1 kHz sampler

constexpr int kMaxThreads = 16;     // Crema: the GPU thread, 3 emulated cores, the recompiler; rpcs3: + SPU threads
constexpr int kMaxSamples = 16384;  // per thread and window (10 s at 1 kHz fits)
// For samples inside a system library (a wait, a sleep), the eboot return addresses that led
// there - who is waiting, and for what. RPCS3's busy threads spent most samples in libkernel's
// _umtx_op (God of War, rpcs3 v01.37), which alone says nothing.
constexpr int kWaitPcs = 4;
constexpr int kMaxWaitStacks = 8192;

// Allocated when the sampler is first switched on (384 KiB a thread: nothing while it is off).
struct SampleBuffers {
    uint64_t samples[kMaxSamples];
    uint64_t wait_stacks[kMaxWaitStacks][kWaitPcs];
};

struct ThreadSamples {
    std::atomic<pthread_t> thread{};
    char name[32] = {};  // copied: some callers pass temporaries
    // its stack: the scan stays inside it (0: unknown, to the end of rsp's page). A slot whose thread
    // ended can match a new thread with the same pthread_t: then rsp is outside and the scan stays in
    // rsp's page.
    uintptr_t stack_start = 0, stack_end = 0;
    std::atomic<SampleBuffers*> buffers{nullptr};
    std::atomic<int> count{0};
    std::atomic<int> wait_count{0};
};

ThreadSamples g_threads[kMaxThreads];
std::atomic<int> g_thread_count{0};
std::atomic<bool> g_sampler_on{false}, g_sampler_started{false}, g_code_dumps{true};

void allocateBuffers(ThreadSamples& t) {
    if (t.buffers.load(std::memory_order_acquire))
        return;
    auto* buffers = static_cast<SampleBuffers*>(calloc(1, sizeof(SampleBuffers)));
    SampleBuffers* expected = nullptr;
    if (buffers && !t.buffers.compare_exchange_strong(expected, buffers))
        free(buffers);  // (another thread allocated them first)
}

// The eboot return addresses a system-library sample came from, nearest first (relative to .text):
// return addresses only (a call right before them; the word at rsp is the exact caller of a system
// call stub, which has no frame - M33), and only inside the thread's stack: reading past its end
// crashed rpcs3 as DMC4 stopped (v01.51). On a stack that isn't the registered one (a fiber's, or a
// new thread that inherited the slot's pthread_t) only to the end of rsp's 16 KiB page.
void waitChain(const ThreadSamples& t, uint64_t rsp, uint64_t* pcs) {
    int found = 0;
    const bool inside = t.stack_end && rsp >= t.stack_start && rsp < t.stack_end;
    const uintptr_t end = inside ? t.stack_end : (rsp | (kPage - 1)) + 1;
    const auto* stack = reinterpret_cast<const uint64_t*>(rsp & ~7ull);
    const int words = static_cast<int>(std::min<uintptr_t>(256, (end - (rsp & ~7ull)) / 8));
    for (int k = 0; k < words && found < kWaitPcs; k++) {
        const uint64_t value = stack[k];
        if (isReturnAddress(value))
            pcs[found++] = toElf(value);
    }
    for (; found < kWaitPcs; found++)
        pcs[found] = 0;
}

void profHandler(int, siginfo_t*, void* context) {
    const pthread_t self = pthread_self();
    const int threads = std::min(g_thread_count.load(std::memory_order_acquire), kMaxThreads);
    for (int i = 0; i < threads; i++) {
        ThreadSamples& t = g_threads[i];
        if (t.thread.load(std::memory_order_relaxed) != self)
            continue;
        SampleBuffers* b = t.buffers.load(std::memory_order_acquire);
        if (!b)
            return;
        const uint64_t* regs = regsOf(context);
        const int n = t.count.load(std::memory_order_relaxed);
        if (n < kMaxSamples) {
            uint64_t sample = regs[kRipIndex];
            // Inside a system module: keep the exact address (tagged). The module list at the top of
            // samples.log maps it to a module offset, which the decrypted modules' symbol tables (and
            // libkernel's entry points) name.
            if (isSystem(sample)) {
                sample |= kSystemTag;
                const int w = t.wait_count.load(std::memory_order_relaxed);
                if (w < kMaxWaitStacks) {
                    waitChain(t, regs[kRspIndex], b->wait_stacks[w]);
                    t.wait_count.store(w + 1, std::memory_order_release);
                }
            }
            b->samples[n] = sample;
            t.count.store(n + 1, std::memory_order_release);
        }
        return;
    }
}

// Scratch for the reports, allocated by the sampler's thread when it starts.
struct ReportScratch {
    struct Entry {
        uint64_t key;
        int count;
    };
    struct Chain {
        uint64_t pcs[kWaitPcs];
    };
    struct ChainEntry {
        int first;
        int count;
    };
    uint64_t sorted[kMaxSamples];
    Entry entries[kMaxSamples];
    Chain chains[kMaxWaitStacks];
    ChainEntry chain_entries[kMaxWaitStacks];
};

// JIT code seen in the samples since the last dump: 64 KiB chunks, and (with ps4_jit_owner) the guest
// functions (Crema: hotfuncs.bin).
constexpr int kMaxHotChunks = 96;
uint64_t g_hot_chunks[kMaxHotChunks];
int g_hot_chunk_count = 0;

void noteHotJit(uint64_t address, int count) {
    if (count < 3)
        return;
    const uint64_t chunk = address & ~0xFFFFULL;
    for (int i = 0; i < g_hot_chunk_count; i++) {
        if (g_hot_chunks[i] == chunk)
            return;
    }
    if (g_hot_chunk_count < kMaxHotChunks)
        g_hot_chunks[g_hot_chunk_count++] = chunk;
}

constexpr int kMaxHotFunctions = 512;
struct HotFunction {
    uint32_t guest;
    uint64_t host;
    int count;
};
HotFunction g_hot_functions[kMaxHotFunctions];
int g_hot_function_count = 0;

void noteHotFunction(uint32_t guest, uint64_t host, int count) {
    for (int i = 0; i < g_hot_function_count; i++) {
        if (g_hot_functions[i].host == host) {
            g_hot_functions[i].count += count;
            return;
        }
    }
    if (g_hot_function_count < kMaxHotFunctions)
        g_hot_functions[g_hot_function_count++] = {guest, host, count};
}

// Aggregates one thread's window: sorts the samples and prints the top entries.
void report(int fd, ThreadSamples& t, double window_start, ReportScratch& r) {
    SampleBuffers* b = t.buffers.load(std::memory_order_acquire);
    if (!b)
        return;
    // Copy first: the handler keeps writing into the buffer once count is reset.
    uint64_t* s = r.sorted;
    const int n = std::min(t.count.load(std::memory_order_acquire), kMaxSamples);
    memcpy(s, b->samples, sizeof(uint64_t) * n);
    t.count.store(0, std::memory_order_release);
    if (n == 0)
        return;
    // Eboot and JIT code: bucket by 16 bytes (eboot addresses relative to .text).
    for (int i = 0; i < n; i++) {
        if (s[i] & kSystemTag)
            s[i] &= ~0xFULL;
        else
            s[i] = isCode(s[i]) ? (toElf(s[i]) & ~0xFULL) : ((s[i] | kJitTag) & ~0xFULL);
    }
    std::sort(s, s + n);
    int unique = 0, eboot_samples = 0;
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && s[j] == s[i])
            j++;
        r.entries[unique++] = {s[i], j - i};
        if (!(s[i] >> 62))
            eboot_samples += j - i;
        i = j;
    }
    std::sort(r.entries, r.entries + unique, [](const auto& a, const auto& c) { return a.count > c.count; });

    char line[256];
    snprintf(line, sizeof(line), "== %.0f s, %s: %d samples, %d in eboot code (%.0f%%)\n", window_start, t.name, n,
             eboot_samples, 100.0 * eboot_samples / n);
    writeLine(fd, line);
    // Every bucket seen at least twice (up to 1500): a function spread over many 16-byte buckets
    // fell below a top-60 cut, hiding about half of a busy thread. Aggregated per function offline
    // (symbolizer, JIT block map).
    for (int i = 0; i < unique && i < 1500 && r.entries[i].count >= 2; i++) {
        const uint64_t key = r.entries[i].key;
        const double percent = 100.0 * r.entries[i].count / n;
        if (key & kJitTag) {
            const uint64_t address = key & ~kJitTag;
            uint32_t guest = 0;
            uint64_t host = 0;
            if (ps4_jit_owner && ps4_jit_owner(address, &guest, &host)) {
                snprintf(line, sizeof(line), "  %5.1f%% jit 0x%llx guest %08x +%llx\n", percent,
                         static_cast<unsigned long long>(address), guest, static_cast<unsigned long long>(address - host));
                noteHotFunction(guest, host, r.entries[i].count);
            } else {
                snprintf(line, sizeof(line), "  %5.1f%% jit 0x%llx\n", percent, static_cast<unsigned long long>(address));
            }
            noteHotJit(address, r.entries[i].count);
        } else if (key & kSystemTag) {
            snprintf(line, sizeof(line), "  %5.1f%% sys 0x%llx\n", percent,
                     static_cast<unsigned long long>(key & ~kSystemTag));
        } else {
            snprintf(line, sizeof(line), "  %5.1f%% elf 0x%llx\n", percent, static_cast<unsigned long long>(key));
        }
        writeLine(fd, line);
    }
}

// The window's samples inside system libraries, grouped by the eboot call chain that led there:
// "  P% wait A B C D" (percent of those samples; addresses relative to .text, nearest first).
void reportWaits(int fd, ThreadSamples& t, ReportScratch& r) {
    SampleBuffers* b = t.buffers.load(std::memory_order_acquire);
    if (!b)
        return;
    const int n = std::min(t.wait_count.load(std::memory_order_acquire), kMaxWaitStacks);
    memcpy(r.chains, b->wait_stacks, sizeof(ReportScratch::Chain) * n);
    t.wait_count.store(0, std::memory_order_release);
    if (n == 0)
        return;
    auto less = [](const ReportScratch::Chain& a, const ReportScratch::Chain& c) {
        return memcmp(a.pcs, c.pcs, sizeof(a.pcs)) < 0;
    };
    std::sort(r.chains, r.chains + n, less);
    int unique = 0;
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && memcmp(r.chains[j].pcs, r.chains[i].pcs, sizeof(r.chains[i].pcs)) == 0)
            j++;
        r.chain_entries[unique++] = {i, j - i};
        i = j;
    }
    std::sort(r.chain_entries, r.chain_entries + unique, [](const auto& a, const auto& c) { return a.count > c.count; });
    char line[256];
    snprintf(line, sizeof(line), "-- waits of %s: %d samples\n", t.name, n);
    writeLine(fd, line);
    for (int i = 0; i < unique && i < 20 && r.chain_entries[i].count >= 2; i++) {
        const uint64_t* pcs = r.chains[r.chain_entries[i].first].pcs;
        snprintf(line, sizeof(line), "  %5.1f%% wait 0x%llx 0x%llx 0x%llx 0x%llx\n", 100.0 * r.chain_entries[i].count / n,
                 static_cast<unsigned long long>(pcs[0]), static_cast<unsigned long long>(pcs[1]),
                 static_cast<unsigned long long>(pcs[2]), static_cast<unsigned long long>(pcs[3]));
        writeLine(fd, line);
    }
}

// jitcode.bin: records of {u64 address, u64 size, bytes}. Copied through a buffer first so the
// kernel never reads JIT memory for the file write. Only written when there is something to write
// (the ports wrote an empty jitcode.bin every minute in builds without a JIT).
bool dumpCode() {
    bool wrote = false;
    if (g_hot_chunk_count > 0) {
        const int fd = createDataFile("jitcode.bin");
        if (fd >= 0) {
            static uint8_t buffer[0x10000];
            for (int i = 0; i < g_hot_chunk_count; i++) {
                if (!isReadable(g_hot_chunks[i], sizeof(buffer)))
                    continue;
                memcpy(buffer, reinterpret_cast<const void*>(g_hot_chunks[i]), sizeof(buffer));
                const uint64_t header[2] = {g_hot_chunks[i], sizeof(buffer)};
                write(fd, header, sizeof(header));
                write(fd, buffer, sizeof(buffer));
            }
            close(fd);
            wrote = true;
        }
        g_hot_chunk_count = 0;
    }
    if (ps4_profile_guest_ram && ps4_profile_guest_ram_size) {
        const int ram = createDataFile("mem1.bin");
        if (ram >= 0) {
            static uint8_t copy[0x100000];
            for (uint32_t offset = 0; offset < ps4_profile_guest_ram_size; offset += sizeof(copy)) {
                const uint32_t size = std::min<uint32_t>(sizeof(copy), ps4_profile_guest_ram_size - offset);
                memcpy(copy, static_cast<const uint8_t*>(ps4_profile_guest_ram) + offset, size);
                write(ram, copy, size);
            }
            close(ram);
            wrote = true;
        }
    }
    // the busiest guest functions with their guest and host code (the port's ps4_jit_dump_function)
    if (ps4_jit_dump_function && g_hot_function_count > 0) {
        std::sort(g_hot_functions, g_hot_functions + g_hot_function_count,
                  [](const HotFunction& a, const HotFunction& b) { return a.count > b.count; });
        const int fd = createDataFile("hotfuncs.bin");
        if (fd >= 0) {
            for (int i = 0; i < std::min(g_hot_function_count, 60); i++)
                ps4_jit_dump_function(fd, g_hot_functions[i].guest, g_hot_functions[i].host,
                                      static_cast<uint32_t>(g_hot_functions[i].count));
            close(fd);
            wrote = true;
        }
    }
    return wrote;
}

void* samplerThread(void*) {
    const int fd = createDataFile("samples.log");
    auto* scratch = static_cast<ReportScratch*>(malloc(sizeof(ReportScratch)));
    if (fd < 0 || !scratch) {
        ps4_boot_trace("sampler: cannot write samples.log, not started");
        return nullptr;
    }
    size_t listed = 0;
    writeModuleList(fd, &listed);
    const double start = now();
    double window_start = start, last_dump = start;
    const timespec period = {0, 1000000}, paused = {0, 100000000};
    for (;;) {
        // Profiler switched off in the menus: no signals, no new windows until it is back on.
        if (!g_sampler_on.load(std::memory_order_relaxed)) {
            nanosleep(&paused, nullptr);
            window_start = now();
            continue;
        }
        nanosleep(&period, nullptr);
        const int threads = std::min(g_thread_count.load(std::memory_order_acquire), kMaxThreads);
        for (int i = 0; i < threads; i++) {
            const pthread_t thread = g_threads[i].thread.load(std::memory_order_relaxed);
            if (thread != pthread_t{} && g_threads[i].buffers.load(std::memory_order_relaxed))
                pthread_kill(thread, kSigProf);
        }
        if (now() - window_start >= 10.0) {
            writeModuleList(fd, &listed);
            for (int i = 0; i < threads; i++) {
                report(fd, g_threads[i], window_start - start, *scratch);
                reportWaits(fd, g_threads[i], *scratch);
            }
            window_start = now();
            if (window_start - last_dump >= 60.0) {
                last_dump = window_start;
                if (g_code_dumps.load(std::memory_order_relaxed) && dumpCode())
                    writeLine(fd, "(jitcode.bin / mem1.bin / hotfuncs.bin updated)\n");
                window_start = now();
            }
        }
    }
    return nullptr;
}

}  // namespace

extern "C" void ps4_sampler_register_thread(const char* name) {
    ps4_watch_thread(name);
    const pthread_t self = pthread_self();
    void* stack_addr = nullptr;
    size_t stack_size = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (pthread_attr_get_np(self, &attr) == 0)
        pthread_attr_getstack(&attr, &stack_addr, &stack_size);
    pthread_attr_destroy(&attr);
    // A thread that is started again (M33, Resonance: the emulation thread at every game start)
    // takes over its predecessor's slot instead of a new one.
    int index = -1;
    const int count = std::min(g_thread_count.load(), kMaxThreads);
    for (int i = 0; i < count && index < 0; i++) {
        if (name && strncmp(g_threads[i].name, name, sizeof(g_threads[i].name) - 1) == 0)
            index = i;
    }
    if (index < 0) {
        index = g_thread_count.fetch_add(1);  // (two threads registering at once get two slots)
        if (index >= kMaxThreads)
            return;
    }
    ThreadSamples& t = g_threads[index];
    t.thread.store(pthread_t{});  // (no samples while the slot changes hands)
    snprintf(t.name, sizeof(t.name), "%s", name ? name : "?");
    t.stack_start = reinterpret_cast<uintptr_t>(stack_addr);
    t.stack_end = stack_size ? reinterpret_cast<uintptr_t>(stack_addr) + stack_size : 0;
    if (g_sampler_started.load())
        allocateBuffers(t);
    t.thread.store(self, std::memory_order_release);
}

extern "C" void ps4_sampler_set_enabled(int on) {
    if (!on) {
        if (g_sampler_on.exchange(false))
            ps4_boot_trace("sampler: paused");
        return;
    }
    g_sampler_on.store(true);
    if (g_sampler_started.exchange(true))
        return;
    const int count = std::min(g_thread_count.load(), kMaxThreads);
    for (int i = 0; i < count; i++)
        allocateBuffers(g_threads[i]);
    installHandler(kSigProf, profHandler);
    pthread_t thread;
    if (pthread_create(&thread, nullptr, samplerThread, nullptr) == 0)
        ps4_boot_trace("sampler: started, writing samples.log every 10 s");
}

extern "C" void ps4_sampler_start() { ps4_sampler_set_enabled(1); }
extern "C" int ps4_sampler_enabled() { return g_sampler_on.load() ? 1 : 0; }

extern "C" void ps4_sampler_set_guest_ram(void* base, uint32_t size) {
    ps4_profile_guest_ram_size = 0;
    ps4_profile_guest_ram = base;
    ps4_profile_guest_ram_size = base ? size : 0;
}

extern "C" void ps4_sampler_set_code_dumps(int on) { g_code_dumps.store(on != 0); }

// ---------------------------------------------------------------------------------------------
// Thread dumps: every thread that matters registers here (ps4_watch_thread, also done by
// ps4_sampler_register_thread). ps4_dump_thread_stacks() signals each one; the handler records its
// instruction pointer and the return addresses on its stack (newest first). Used by the app's
// watchdog when it stops advancing, and by the crash handler.
namespace {
constexpr int kMaxWatched = 128, kMaxFrames = 48;  // every thread of the app (pthread_create wrap)
// A thread away from its registered stack (Crema's CPU threads run the emulated threads on fiber
// stacks): this much above rsp is scanned, within its mapping.
constexpr uintptr_t kForeignStackScan = 16 * 1024;

struct WatchedThread {
    std::atomic<pthread_t> thread{};
    char name[32] = {};  // copied: some callers pass temporaries
    // Highest stack address the dump may read: the thread's stack position when it registered (plus
    // its caller's frame). Reading a fixed amount above rsp ran off the top of small thread stacks
    // (Dolphin's Wiimote scanner) and crashed the app in the middle of a hang dump.
    uintptr_t stack_limit = 0;
    int tid = 0;  // kernel thread id (pthread_getthreadid_np): mutex objects record their owner's
    uint64_t rip = 0;
    uint64_t frames[kMaxFrames];
    int frame_count = 0;
    // Blocked in _umtx_op (the kernel's wait for mutexes, condition variables, semaphores): its
    // arguments - object, operation, value, second object (condition variable waits: the mutex) -
    // and the first 16 bytes of both objects (a FreeBSD mutex starts with its owner's thread id).
    bool in_umtx = false;
    uint64_t umtx_obj = 0, umtx_op = 0, umtx_val = 0, umtx_obj2 = 0;
    uint32_t umtx_words[4] = {}, umtx_words2[4] = {};
    std::atomic<int> done{0};
};
WatchedThread g_watched[kMaxWatched];
std::atomic<int> g_watched_count{0};
std::atomic<bool> g_thread_dumps{true};
// Self-reported CPU time per slot (ps4_thread_cpu_tick, below)
std::atomic<uint64_t> g_watched_cpu_ns[kMaxWatched];

// A slot is cleared when its thread exits (a pthread key destructor) and reused by the next thread.
// RPCS3 starts a thread per compiled module - hundreds - and the 96 slots were full long before the
// game's own threads started; the busy report also signals every watched thread, which must never
// reach one that is gone. The generation tells the report a slot has a new thread (rpcs3).
std::atomic<uint32_t> g_watched_gen[kMaxWatched];
pthread_key_t g_unwatch_key;
std::atomic<int> g_unwatch_key_state{0};  // 0 none, 1 creating, 2 ready

// The dump signal has two users (thread dumps, the hot-spot profiler) sharing each slot's fields:
// one at a time. Bounded: the crash handler's dump never waits more than 50 ms for it.
std::atomic<bool> g_dump_busy{false};
bool lockDumps(int max_ms) {
    const timespec pause = {0, 1000000};
    for (int i = 0; g_dump_busy.exchange(true, std::memory_order_acquire); i++) {
        if (i >= max_ms)
            return false;
        nanosleep(&pause, nullptr);
    }
    return true;
}
void unlockDumps() { g_dump_busy.store(false, std::memory_order_release); }

void unwatchThread(void* value) {
    const int i = static_cast<int>(reinterpret_cast<uintptr_t>(value)) - 1;
    if (i < 0 || i >= kMaxWatched)
        return;
    g_watched_cpu_ns[i].store(0, std::memory_order_relaxed);
    g_watched_gen[i].fetch_add(1, std::memory_order_release);
    g_watched[i].thread.store(pthread_t{}, std::memory_order_release);
}

void setUnwatchOnExit(int index) {
    int state = 0;
    if (g_unwatch_key_state.compare_exchange_strong(state, 1)) {
        pthread_key_create(&g_unwatch_key, unwatchThread);
        g_unwatch_key_state.store(2, std::memory_order_release);
    }
    while (g_unwatch_key_state.load(std::memory_order_acquire) != 2)
        sched_yield();
    pthread_setspecific(g_unwatch_key, reinterpret_cast<void*>(static_cast<uintptr_t>(index) + 1));
}

void dumpHandler(int, siginfo_t*, void* context) {
    const pthread_t self = pthread_self();
    const int count = std::min(g_watched_count.load(std::memory_order_acquire), kMaxWatched);
    for (int i = 0; i < count; i++) {
        WatchedThread& t = g_watched[i];
        if (t.thread.load(std::memory_order_relaxed) != self)
            continue;
        const uint64_t* regs = regsOf(context);
        t.rip = regs[kRipIndex];
        // libkernel's _umtx_op stub: "mov $0x1c6, %rax; mov %rcx, %r10; syscall" (12 bytes), the
        // interrupted thread sits right after the syscall. Only then are the argument registers
        // known to be the wait's.
        t.in_umtx = false;
        if (isSystem(t.rip) && (t.rip & 0xFFF) >= 12) {
            const auto* code = reinterpret_cast<const uint8_t*>(t.rip - 12);
            static const uint8_t kUmtxStub[12] = {0x48, 0xc7, 0xc0, 0xc6, 0x01, 0x00, 0x00, 0x49, 0x89, 0xca, 0x0f, 0x05};
            if (memcmp(code, kUmtxStub, sizeof(kUmtxStub)) == 0) {
                // Registers only: the objects are read by the dumping thread, after checking that
                // they are mapped (a register that wasn't a pointer crashed Crema v02.37 here).
                t.in_umtx = true;
                t.umtx_obj = regs[kRdiIndex];
                t.umtx_op = regs[kRsiIndex];
                t.umtx_val = regs[kRdxIndex];
                t.umtx_obj2 = regs[kR10Index];
            }
        }
        // Only scan a stack that is plausibly this registration's: a thread that registered and
        // exited can leave its pthread handle to a new thread, whose stack is elsewhere (a Crema
        // v02.34 hang dump crashed scanning from one thread's rsp to another's stack limit).
        const uint64_t rsp = regs[kRspIndex] & ~7ull;
        const bool same_thread = pthread_getthreadid_np() == t.tid;
        const bool sane = t.stack_limit > rsp && t.stack_limit - rsp <= (1u << 20);
        uintptr_t limit_address = rsp;
        if (same_thread && sane) {
            limit_address = t.stack_limit;
        } else if (same_thread) {
            void* start = nullptr;
            void* end = nullptr;
            uint32_t protection = 0;
            if (ps4k_sceKernelQueryMemoryProtection(reinterpret_cast<void*>(rsp), &start, &end, &protection) == 0 &&
                (protection & 1))
                limit_address = std::min<uintptr_t>(rsp + kForeignStackScan, reinterpret_cast<uintptr_t>(end));
        }
        const auto* limit = reinterpret_cast<const uint64_t*>(limit_address);
        int n = 0;
        for (const uint64_t* p = reinterpret_cast<const uint64_t*>(rsp); p < limit && n < kMaxFrames; p++) {
            if (isReturnAddress(*p))
                t.frames[n++] = toElf(*p);
        }
        t.frame_count = n;
        t.done.store(1, std::memory_order_release);
        return;
    }
}

void installDumpHandler() {
    static std::atomic<bool> installed{false};
    if (!installed.exchange(true))
        installHandler(kSigDump, dumpHandler);
}

// Signals a watched thread and waits up to `max_us` for its handler (true: rip and frames are fresh).
bool lookAt(WatchedThread& t, int max_us) {
    const pthread_t thread = t.thread.load(std::memory_order_acquire);
    if (thread == pthread_t{})
        return false;
    t.done.store(0);
    if (pthread_kill(thread, kSigDump) != 0)
        return false;
    const timespec pause = {0, 200000};
    for (int waited = 0; waited < max_us && !t.done.load(std::memory_order_acquire); waited += 200)
        nanosleep(&pause, nullptr);
    return t.done.load(std::memory_order_acquire) != 0;
}

// Copies 16 bytes at `address` if they lie in readable memory (else leaves zeros).
void readIfMapped(uint64_t address, uint32_t (&words)[4]) {
    memset(words, 0, sizeof(words));
    if (isReadable(address, 16))
        memcpy(words, reinterpret_cast<const void*>(address), sizeof(words));
}
}  // namespace

extern "C" void ps4_watch_thread(const char* name) {
    installDumpHandler();
    if (ps4_heap_private_arena)
        ps4_heap_private_arena(name);
    const pthread_t self = pthread_self();
    for (int i = 0; i < std::min(g_watched_count.load(), kMaxWatched); i++) {
        if (g_watched[i].thread.load() == self) {
            // already watched (every thread is, from its start): the name it gives itself
            if (name)
                snprintf(g_watched[i].name, sizeof(g_watched[i].name), "%s", name);
            return;
        }
    }
    // A slot an exited thread left, else a new one
    int index = -1;
    for (int i = 0; i < std::min(g_watched_count.load(), kMaxWatched) && index < 0; i++) {
        pthread_t empty{};
        if (g_watched_gen[i].load(std::memory_order_acquire) != 0 && g_watched[i].thread.compare_exchange_strong(empty, self))
            index = i;
    }
    if (index < 0) {
        index = g_watched_count.fetch_add(1);
        if (index >= kMaxWatched)
            return;
    }
    setUnwatchOnExit(index);
    snprintf(g_watched[index].name, sizeof(g_watched[index].name), "%s", name ? name : "?");
    // Up to and including this function's own return address (frame + 16): threads that register
    // first thing sit within 256 bytes of their stack's top, and a larger margin read past it.
    // Where a thread is stuck lies deeper than this anyway.
    g_watched[index].stack_limit = reinterpret_cast<uintptr_t>(__builtin_frame_address(0)) + 16;
    g_watched[index].tid = pthread_getthreadid_np();
    g_watched[index].thread.store(self);
}

extern "C" void ps4_diag_set_thread_dumps(int on) { g_thread_dumps.store(on != 0); }
extern "C" int ps4_diag_thread_dumps_enabled() { return g_thread_dumps.load() ? 1 : 0; }

// Writes every watched thread's stack to `path` (appending), headed by `reason`.
extern "C" void ps4_dump_thread_stacks(const char* path, const char* reason) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) {
        char line[200];
        snprintf(line, sizeof(line), "thread dump: cannot open %s (errno %d)", path, errno);
        ps4_boot_trace(line);
        return;
    }
    char line[256];
    snprintf(line, sizeof(line), "==== %s (eboot addresses relative to .text, symbolize with llvm-symbolizer --obj=<elf>)\n",
             reason);
    writeLine(fd, line);
    if (!g_thread_dumps.load()) {
        writeLine(fd, "(thread dumps are off)\n");
        close(fd);
        return;
    }
    const bool locked = lockDumps(50);
    const int self_tid = pthread_getthreadid_np();
    const int count = std::min(g_watched_count.load(std::memory_order_acquire), kMaxWatched);
    for (int i = 0; i < count; i++) {
        WatchedThread& t = g_watched[i];
        if (t.thread.load() == pthread_t{})
            continue;  // exited (slot free)
        if (t.tid == self_tid && t.thread.load() == pthread_self()) {
            snprintf(line, sizeof(line), "-- %s (thread id %d): the thread writing this dump\n", t.name, t.tid);
            writeLine(fd, line);
            continue;
        }
        if (!lookAt(t, 200000)) {
            snprintf(line, sizeof(line), "-- %s: no answer (signals blocked, or the thread is gone)\n", t.name);
            writeLine(fd, line);
            continue;
        }
        const bool in_eboot = isCode(t.rip);
        snprintf(line, sizeof(line), "-- %s (thread id %d): rip %s0x%llx%s\n", t.name, t.tid,
                 in_eboot ? "elf " : "(system/JIT) ", static_cast<unsigned long long>(in_eboot ? toElf(t.rip) : t.rip),
                 inSyscall(t.rip) ? " (in a system call)" : "");
        writeLine(fd, line);
        // in translated guest code: which guest function (the emulator's, Crema: PPCRecompiler.cpp)
        if (!in_eboot && !isSystem(t.rip) && ps4_crash_jit_info)
            ps4_crash_jit_info(fd, t.rip, 1);
        if (t.in_umtx) {
            readIfMapped(t.umtx_obj, t.umtx_words);
            readIfMapped(t.umtx_obj2, t.umtx_words2);
            snprintf(line, sizeof(line),
                     "   blocked in _umtx_op(obj %#llx, op %llu, val %#llx, obj2 %#llx); obj: %08x "
                     "%08x %08x %08x; obj2: %08x %08x %08x %08x\n",
                     static_cast<unsigned long long>(t.umtx_obj), static_cast<unsigned long long>(t.umtx_op),
                     static_cast<unsigned long long>(t.umtx_val), static_cast<unsigned long long>(t.umtx_obj2),
                     t.umtx_words[0], t.umtx_words[1], t.umtx_words[2], t.umtx_words[3], t.umtx_words2[0],
                     t.umtx_words2[1], t.umtx_words2[2], t.umtx_words2[3]);
            writeLine(fd, line);
        }
        writeLine(fd, "   stack:");
        for (int f = 0; f < t.frame_count; f++) {
            snprintf(line, sizeof(line), " 0x%llx", static_cast<unsigned long long>(t.frames[f]));
            writeLine(fd, line);
        }
        writeLine(fd, "\n");
    }
    if (locked)
        unlockDumps();
    // The emulator's view last: it takes locks and allocates, and in a freeze may never return - the
    // host stacks above are already written by then.
    if (ps4_recompiler_status)
        ps4_recompiler_status(fd);
    if (ps4_hang_guest_threads)
        ps4_hang_guest_threads(fd);
    close(fd);
}

// ---------------------------------------------------------------------------------------------
// Per-thread CPU time for the app's monitor: busy threads read their own CPU clock now and then; the
// monitor divides the growth by wall time. Shows how close a thread is to its limit and whether
// something else shares its core (the sampler can't see that).
namespace {
constexpr clockid_t kThreadCpuClock = 14;  // FreeBSD CLOCK_THREAD_CPUTIME_ID
std::atomic<uint64_t> g_thread_cpu_ns[2];
constexpr int kWaitSlots = 16;
std::atomic<long long> g_wait_ns[kWaitSlots];

// Threads that never call ps4_thread_cpu_tick are asked instead: SIGUSR2 makes the signalled thread
// read its own CPU clock into its slot (rpcs3: its threads never tick, the busy report was empty).
std::atomic<uint64_t> g_signal_cpu_ns[kMaxWatched];
std::atomic<int> g_signal_cpu_done[kMaxWatched];

void cpuHandler(int, siginfo_t*, void*) {
    const pthread_t self = pthread_self();
    const int count = std::min(g_watched_count.load(std::memory_order_acquire), kMaxWatched);
    for (int i = 0; i < count; i++) {
        if (g_watched[i].thread.load(std::memory_order_relaxed) != self)
            continue;
        timespec ts;
        if (clock_gettime(kThreadCpuClock, &ts) == 0)
            g_signal_cpu_ns[i].store(static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec,
                                     std::memory_order_relaxed);
        g_signal_cpu_done[i].store(1, std::memory_order_release);
        return;
    }
}

// The CPU clocks of the watched threads that don't report their own: signal them all, then give them
// up to 20 ms to answer (a thread that doesn't is left out of this report)
void querySignalledCpu(int count, bool* asked) {
    static std::atomic<bool> installed{false};
    if (!installed.exchange(true))
        installHandler(kSigCpu, cpuHandler);
    for (int i = 0; i < count; i++) {
        asked[i] = false;
        if (g_watched_cpu_ns[i].load(std::memory_order_relaxed) != 0)
            continue;
        const pthread_t thread = g_watched[i].thread.load(std::memory_order_acquire);
        if (thread == pthread_t{})
            continue;  // exited (slot free)
        g_signal_cpu_done[i].store(0, std::memory_order_relaxed);
        asked[i] = pthread_kill(thread, kSigCpu) == 0;
    }
    const timespec pause = {0, 1000000};
    for (int w = 0; w < 20; w++) {
        bool all = true;
        for (int i = 0; i < count && all; i++)
            all = !asked[i] || g_signal_cpu_done[i].load(std::memory_order_acquire);
        if (all)
            break;
        nanosleep(&pause, nullptr);
    }
}
}  // namespace

extern "C" void ps4_thread_cpu_sample(int slot) {
    timespec ts;
    if (slot < 0 || slot > 1 || clock_gettime(kThreadCpuClock, &ts) != 0)
        return;
    g_thread_cpu_ns[slot].store(static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec),
                                std::memory_order_relaxed);
}

extern "C" double ps4_thread_cpu_seconds(int slot) {
    if (slot < 0 || slot > 1)
        return 0;
    return g_thread_cpu_ns[slot].load(std::memory_order_relaxed) / 1e9;
}

extern "C" void ps4_wait_add(int slot, long long ns) {
    if (slot >= 0 && slot < kWaitSlots && ns > 0)
        g_wait_ns[slot].fetch_add(ns, std::memory_order_relaxed);
}

extern "C" long long ps4_wait_total(int slot) {
    return slot >= 0 && slot < kWaitSlots ? g_wait_ns[slot].load(std::memory_order_relaxed) : 0;
}

// The PS4 doesn't let a thread read another thread's CPU clock (pthread_getcpuclockid fails), so busy
// threads report their own: this, in their loops, stores the thread's CPU time in its watched slot at
// most every ~10 ms (rdtsc check; the clock read is a system call).
extern "C" void ps4_thread_cpu_tick() {
    static thread_local int t_slot = -2;
    static thread_local uint32_t t_gen = 0;
    static thread_local uint64_t t_last = 0;
    const uint64_t tsc = __builtin_ia32_rdtsc();
    if (tsc - t_last < 16000000)  // ~10 ms at 1.6 GHz
        return;
    t_last = tsc;
    // (looked up again when the slot changed hands: a reused slot's index may now be another's)
    if (t_slot >= 0 && g_watched_gen[t_slot].load(std::memory_order_relaxed) != t_gen)
        t_slot = -2;
    if (t_slot == -2) {
        t_slot = -1;
        const pthread_t self = pthread_self();
        for (int i = 0; i < std::min(g_watched_count.load(), kMaxWatched); i++) {
            if (g_watched[i].thread.load() == self) {
                t_slot = i;
                t_gen = g_watched_gen[i].load(std::memory_order_relaxed);
            }
        }
    }
    timespec ts;
    if (t_slot >= 0 && clock_gettime(kThreadCpuClock, &ts) == 0)
        g_watched_cpu_ns[t_slot].store(static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec,
                                       std::memory_order_relaxed);
}

// How busy each watched thread was since the previous call: its self-reported CPU time
// (ps4_thread_cpu_tick), else its answer to SIGUSR2, else its CPU clock read from here (where the
// system allows it). "name N%" for those at or above min_percent, busiest first, into `out`. One
// caller (the monitor); no allocations.
extern "C" void ps4_thread_busy_report(char* out, size_t size, int min_percent) {
    static uint64_t s_prev_cpu[kMaxWatched];
    static uint32_t s_prev_gen[kMaxWatched];
    static uint64_t s_prev_wall = 0;
    if (!out || size == 0)
        return;
    timespec wall_now;
    clock_gettime(CLOCK_MONOTONIC, &wall_now);
    const uint64_t wall = static_cast<uint64_t>(wall_now.tv_sec) * 1000000000ull + wall_now.tv_nsec;
    const uint64_t elapsed = s_prev_wall ? wall - s_prev_wall : 0;
    s_prev_wall = wall;
    int percents[kMaxWatched];
    const int count = std::min(g_watched_count.load(std::memory_order_acquire), kMaxWatched);
    bool asked[kMaxWatched];
    querySignalledCpu(count, asked);
    for (int i = 0; i < count; i++) {
        percents[i] = -1;
        // a new thread in this slot: its first reading is the baseline
        if (const uint32_t gen = g_watched_gen[i].load(std::memory_order_acquire); gen != s_prev_gen[i]) {
            s_prev_gen[i] = gen;
            s_prev_cpu[i] = 0;
        }
        if (g_watched[i].thread.load(std::memory_order_acquire) == pthread_t{})
            continue;
        uint64_t ns = g_watched_cpu_ns[i].load(std::memory_order_relaxed);
        if (ns == 0 && asked[i] && g_signal_cpu_done[i].load(std::memory_order_acquire))
            ns = g_signal_cpu_ns[i].load(std::memory_order_relaxed);
        if (ns == 0) {
            clockid_t clock;
            timespec cpu;
            if (pthread_getcpuclockid(g_watched[i].thread.load(), &clock) != 0 || clock_gettime(clock, &cpu) != 0)
                continue;
            ns = static_cast<uint64_t>(cpu.tv_sec) * 1000000000ull + cpu.tv_nsec;
        }
        if (elapsed && s_prev_cpu[i] && ns >= s_prev_cpu[i])
            percents[i] = static_cast<int>((ns - s_prev_cpu[i]) * 100 / elapsed);
        s_prev_cpu[i] = ns;
    }
    size_t length = 0;
    out[0] = 0;
    for (;;) {
        int best = -1;
        for (int i = 0; i < count; i++)
            if (percents[i] >= min_percent && (best < 0 || percents[i] > percents[best]))
                best = i;
        if (best < 0 || length + 40 >= size)
            break;
        length += snprintf(out + length, size - length, "%s%s %d%%", length ? ", " : "", g_watched[best].name,
                           percents[best]);
        percents[best] = -1;
    }
}

// ---------------------------------------------------------------------------------------------
// The hot-spot profiler (Conker's Performance > Profiler switch). Built on the thread dump's signal,
// at a low rate: every 20 ms each watched thread that was running code at its last look is looked at
// again; one that was waiting in a system call only once a second, so waiting threads are rarely
// interrupted (the 1 kHz SIGPROF sampler froze SMW's replay thread in a libkernel wait). Every 10 s
// profile.log gets, per thread with samples: how often it ran, and its busiest code (16-byte buckets:
// "elf" = .text-relative; "sys" = a system library, with the eboot code that called it).
namespace {
std::atomic<bool> g_hot_on{false};
std::atomic<bool> g_hot_started{false};
constexpr int kHotMax = 1024;  // samples per thread and window (50 a second at most)

struct HotThread {
    bool running = true;  // at the last look
    int looks = 0, busy = 0;
    uint32_t gen = 0;  // the slot's generation these counts belong to
    uint64_t samples[kHotMax];  // bucket (bit 63: system library), and for those the caller
    uint64_t callers[kHotMax];
    int count = 0;
};
HotThread* g_hot = nullptr;  // [kMaxWatched], allocated when first switched on (2 MiB)

void hotReport(int fd, double at) {
    char line[256];
    snprintf(line, sizeof(line), "======== %.0f s\n", at);
    writeLine(fd, line);
    const int count = std::min(g_watched_count.load(), kMaxWatched);
    static uint64_t keys[kHotMax];
    struct Entry {
        uint64_t key;
        int n;
    };
    static Entry entries[kHotMax];
    for (int i = 0; i < count; i++) {
        HotThread& h = g_hot[i];
        if (h.looks == 0)
            continue;
        snprintf(line, sizeof(line), "-- %s: running in %d of %d looks, %d samples\n", g_watched[i].name, h.busy,
                 h.looks, h.count);
        writeLine(fd, line);
        // busiest buckets, then for system-library samples the callers
        for (int pass = 0; pass < 2 && h.count > 0; pass++) {
            int n = 0;
            for (int s = 0; s < h.count; s++) {
                if (pass == 0)
                    keys[n++] = h.samples[s];
                else if (h.samples[s] >> 63)
                    keys[n++] = h.callers[s];
            }
            if (n == 0)
                continue;
            std::sort(keys, keys + n);
            int unique = 0;
            for (int s = 0; s < n;) {
                int e = s;
                while (e < n && keys[e] == keys[s])
                    e++;
                entries[unique++] = {keys[s], e - s};
                s = e;
            }
            std::sort(entries, entries + unique, [](const Entry& a, const Entry& b) { return a.n > b.n; });
            writeLine(fd, pass == 0 ? "   where:\n" : "   system-library time, called from:\n");
            for (int e = 0; e < unique && e < 40; e++) {
                const uint64_t key = entries[e].key;
                if (pass == 0 && (key >> 63))
                    snprintf(line, sizeof(line), "   %5.1f%% sys 0x%llx\n", 100.0 * entries[e].n / h.count,
                             static_cast<unsigned long long>(key & ~(1ULL << 63)));
                else
                    snprintf(line, sizeof(line), "   %5.1f%% elf 0x%llx\n", 100.0 * entries[e].n / (pass == 0 ? h.count : n),
                             static_cast<unsigned long long>(key));
                writeLine(fd, line);
            }
        }
        h.looks = h.busy = h.count = 0;
    }
}

void* hotThread(void*) {
    ps4_watch_thread("hot profiler");
    const int fd = createDataFile("profile.log");
    if (fd < 0)
        return nullptr;
    writeLine(fd, "hot-spot profile: every 10 s, the busiest code of each thread that ran.\n");
    size_t listed = 0;
    writeModuleList(fd, &listed);
    const timespec period = {0, 20000000}, idle = {0, 200000000};
    const double start = now();
    double window = start;
    for (unsigned round = 0;; round++) {
        if (!g_hot_on.load(std::memory_order_relaxed)) {
            nanosleep(&idle, nullptr);
            window = now();
            continue;
        }
        nanosleep(&period, nullptr);
        const pthread_t self = pthread_self();
        const int count = std::min(g_watched_count.load(std::memory_order_acquire), kMaxWatched);
        for (int i = 0; i < count; i++) {
            WatchedThread& t = g_watched[i];
            HotThread& h = g_hot[i];
            // a new thread in the slot starts as running, with no samples of its predecessor's
            if (const uint32_t gen = g_watched_gen[i].load(std::memory_order_acquire); gen != h.gen) {
                h.gen = gen;
                h.running = true;
                h.looks = h.busy = h.count = 0;
            }
            const pthread_t thread = t.thread.load();
            if (thread == pthread_t{} || thread == self || (!h.running && round % 50 != static_cast<unsigned>(i) % 50))
                continue;
            if (!lockDumps(0))
                break;  // a thread dump is running: this round is skipped
            const bool answered = lookAt(t, 5000);
            const uint64_t rip = t.rip;
            const bool in_umtx = t.in_umtx;
            const uint64_t caller = t.frame_count > 0 ? t.frames[0] : 0;
            unlockDumps();
            if (!answered)
                continue;
            h.looks++;
            h.running = !in_umtx && !inSyscall(rip);
            if (!h.running || h.count >= kHotMax)
                continue;
            h.busy++;
            if (isCode(rip)) {
                h.samples[h.count] = toElf(rip) & ~0xFULL;
                h.callers[h.count] = 0;
            } else {
                h.samples[h.count] = (rip & ~0xFULL) | (1ULL << 63);
                h.callers[h.count] = caller & ~0xFULL;
            }
            h.count++;
        }
        if (now() - window >= 10.0) {
            hotReport(fd, window - start);
            window = now();
        }
    }
    return nullptr;
}
}  // namespace

extern "C" void ps4_hot_profiler_set_enabled(int on) {
    if (on && !g_hot) {
        g_hot = static_cast<HotThread*>(calloc(kMaxWatched, sizeof(HotThread)));
        if (!g_hot) {
            ps4_boot_trace("hot-spot profiler: out of memory, not started");
            return;
        }
        for (int i = 0; i < kMaxWatched; i++)
            g_hot[i].running = true;
    }
    g_hot_on.store(on != 0);
    if (on && !g_hot_started.exchange(true)) {
        installDumpHandler();
        pthread_t thread;
        if (pthread_create(&thread, nullptr, hotThread, nullptr) == 0)
            ps4_boot_trace("hot-spot profiler: started, writing profile.log every 10 s");
    }
}

extern "C" int ps4_hot_profiler_enabled() { return g_hot_on.load() ? 1 : 0; }
