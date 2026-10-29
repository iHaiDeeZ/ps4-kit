// Crash diagnostics for PS4 builds.
//
// PS4 coredumps are encrypted and klog is shared with other tools, so the app reports its own
// crashes: a constructor that runs before all others (priority 101) starts <data dir>/boot-trace.log
// and installs a handler for fatal signals that appends the signal, fault address, registers and
// the return addresses found on the stack to <data dir>/crash.log, then every watched thread's
// stack (ps4_sampler.cpp). Addresses are printed relative to the start of .text, i.e. as ELF
// virtual addresses: symbolize with `llvm-symbolizer --obj=<elf> 0x...` (scripts/crashlog.sh).
//
// Handlers installed later (fastmem, an emulator's own access violation handler) chain to this one
// for faults they don't handle. The app's folder and the files whose paths follow a rename are set
// at link time (ps4_kit_diag.h, "Configuration"); the switches at run time.
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
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cxxabi.h>
#include <exception>
#include <typeinfo>

#ifndef PS4_KIT_DATA_DIR
#define PS4_KIT_DATA_DIR "/data/ps4-kit"
#endif

using namespace ps4diag;

// The port's configuration (ps4_kit_diag.h) and hooks: all optional. Hidden, so a missing one is
// resolved to null at link time instead of becoming a dynamic import the PS4 loader can't satisfy.
#define PS4_WEAK extern "C" __attribute__((weak, visibility("hidden")))
PS4_WEAK const char ps4_diag_config_data_dir[];
PS4_WEAK const char ps4_diag_config_old_data_dir[];
PS4_WEAK const char* const ps4_diag_config_migrate_files[];
PS4_WEAK const char ps4_diag_config_game_marker[];
PS4_WEAK const char ps4_diag_config_game_stop_file[];
PS4_WEAK void ps4_crash_guest_info();
PS4_WEAK void ps4_crash_guest_regs(int fd);
PS4_WEAK void ps4_crash_jit_info(int fd, uint64_t rip, int brief);
PS4_WEAK void ps4_crash_note();
PS4_WEAK void ps4_crash_restart();
// The system's (ps4_runtime.cpp wraps these: pthread_create allocates, which a crash may have left
// impossible; open may go to the network file layer). Without --wrap they are null and the plain
// functions are used.
PS4_WEAK int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
PS4_WEAK int __real_open(const char* path, int flags, ...);
PS4_WEAK int __real_close(int fd);

extern "C" int pthread_getthreadid_np(void);  // libkernel (FreeBSD libthr)
extern "C" int32_t ps4k_sceKernelMprotect(const void* addr, size_t len, int prot) __asm__("sceKernelMprotect");

namespace {

int rawOpen(const char* path, int flags, int mode = 0) {
    return __real_open ? __real_open(path, flags, mode) : open(path, flags, mode);
}
int rawClose(int fd) { return __real_close ? __real_close(fd) : close(fd); }
int rawThread(pthread_t* thread, void* (*start)(void*)) {
    return __real_pthread_create ? __real_pthread_create(thread, nullptr, start, nullptr)
                                 : pthread_create(thread, nullptr, start, nullptr);
}

std::atomic<bool> g_crashLogOn{true}, g_bootTraceOn{true}, g_openTrackingOn{true};

// Back into the app's menu if it has one, else the process ends.
[[noreturn]] void endAfterCrash(int status) {
    if (ps4_crash_restart)
        ps4_crash_restart();
    _exit(status);
}

void writeAll(int fd, const char* text) {
    size_t length = strlen(text);
    while (length > 0) {
        const ssize_t written = write(fd, text, length);
        if (written <= 0)
            return;
        text += written;
        length -= static_cast<size_t>(written);
    }
}

__attribute__((format(printf, 2, 3))) void writeFormat(int fd, const char* fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    writeAll(fd, line);
}

bool pathExists(const char* path) {
    struct stat info;
    return stat(path, &info) == 0;
}

// ---------------------------------------------------------------------------------------------
// The app's folder. Worked out at the first file the app writes (the boot trace, which the heap may
// write before anything else has run: no heap here), with the crash files' paths.

std::atomic<int> g_dirState{0};  // 0 not yet, 1 being worked out, 2 ready
int g_dataMoved = 0;
char g_dataDir[96];
char g_crashLogPath[128], g_bootTracePath[128];

// After the old folder was renamed: the full paths kept in the app's own files (game lists,
// settings) changed to the new place. Streamed through a small buffer into a new file that then
// replaces the old one (the ports read the whole file into a static 1 MiB buffer).
void rewritePaths(const char* path, const char* from, const char* to) {
    const int in = rawOpen(path, O_RDONLY);
    if (in < 0)
        return;
    char temp[160];
    snprintf(temp, sizeof(temp), "%s.moving", path);
    const int out = rawOpen(temp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) {
        rawClose(in);
        return;
    }
    const size_t fromLength = strlen(from), toLength = strlen(to);
    char buffer[8192 + 160];
    size_t have = 0;
    bool ok = fromLength > 0 && fromLength < 160;
    for (bool end = false; ok && !end;) {
        const ssize_t n = read(in, buffer + have, 8192);
        if (n < 0) {
            ok = false;
            break;
        }
        end = n == 0;
        have += static_cast<size_t>(n);
        size_t i = 0, emitted = 0;
        while (i + fromLength <= have) {
            if (memcmp(buffer + i, from, fromLength) == 0) {
                write(out, buffer + emitted, i - emitted);
                write(out, to, toLength);
                i += fromLength;
                emitted = i;
            } else {
                i++;
            }
        }
        // keep a tail that may be the start of a match continuing in the next block
        size_t keep = end ? have : (have >= fromLength - 1 ? have - (fromLength - 1) : 0);
        if (keep < emitted)
            keep = emitted;
        write(out, buffer + emitted, keep - emitted);
        memmove(buffer, buffer + keep, have - keep);
        have -= keep;
    }
    rawClose(in);
    rawClose(out);
    if (ok)
        rename(temp, path);
    else
        unlink(temp);
}

void resolveDataDir() {
    int state = 0;
    if (!g_dirState.compare_exchange_strong(state, 1)) {
        while (g_dirState.load(std::memory_order_acquire) != 2)
            sched_yield();
        return;
    }
    const char* dir = ps4_diag_config_data_dir && ps4_diag_config_data_dir[0] ? ps4_diag_config_data_dir
                                                                              : PS4_KIT_DATA_DIR;
    const char* old = ps4_diag_config_old_data_dir && ps4_diag_config_old_data_dir[0] ? ps4_diag_config_old_data_dir
                                                                                      : nullptr;
    // The app was renamed (rcps3 -> rpcs3 v01.23, CemuPS4 -> Crema v01.34, Vita3K -> HENkaku 1.00,
    // DeCast-S4 -> Resonance): its folder - games, firmware, saves, settings - goes with it once,
    // before anything opens a file there. If that fails the old folder stays in use.
    if (old && !pathExists(dir) && pathExists(old)) {
        if (rename(old, dir) == 0) {
            g_dataMoved = 1;
            if (ps4_diag_config_migrate_files) {
                for (const char* const* name = ps4_diag_config_migrate_files; *name; name++) {
                    char path[160];
                    snprintf(path, sizeof(path), "%s/%s", dir, *name);
                    rewritePaths(path, old, dir);
                }
            }
        } else if (!pathExists(dir)) {
            dir = old;
            g_dataMoved = -1;
        }
    }
    snprintf(g_dataDir, sizeof(g_dataDir), "%s", dir);
    mkdir(g_dataDir, 0777);
    snprintf(g_crashLogPath, sizeof(g_crashLogPath), "%s/crash.log", g_dataDir);
    snprintf(g_bootTracePath, sizeof(g_bootTracePath), "%s/boot-trace.log", g_dataDir);
    g_dirState.store(2, std::memory_order_release);
}

// ---------------------------------------------------------------------------------------------
// The boot trace. Opened on first use: the heap may log before this file's constructor runs.

std::atomic<int> g_traceFd{-1};
std::atomic<int> g_traceState{0};  // 0 not yet, 1 being opened, 2 done (open or failed)
std::atomic<int> g_traceOpener{0};
timespec g_traceStart;

int traceFd() {
    if (g_traceState.load(std::memory_order_acquire) == 2)
        return g_traceFd.load(std::memory_order_relaxed);
    int state = 0;
    if (!g_traceState.compare_exchange_strong(state, 1)) {
        // being opened: by this thread (something on the way logs) - not yet; by another - wait
        if (g_traceOpener.load(std::memory_order_relaxed) == pthread_getthreadid_np())
            return -1;
        while (g_traceState.load(std::memory_order_acquire) != 2)
            sched_yield();
        return g_traceFd.load(std::memory_order_relaxed);
    }
    g_traceOpener.store(pthread_getthreadid_np(), std::memory_order_relaxed);
    resolveDataDir();
    // The last run's trace is kept: as boot-trace-game.log after a game ran (its trace names the game
    // early on, or the app left its "game stopped" file: the launcher starts after a game and would
    // overwrite it; Crema, rpcs3), else as boot-trace-prev.log (a game killed by the system leaves no
    // crash.log; HENkaku v00.04: a quick second start had wiped the trace that mattered).
    char previousPath[160];
    bool gameRun = false;
    if (ps4_diag_config_game_marker && ps4_diag_config_game_marker[0]) {
        static char head[2048];
        const int previous = rawOpen(g_bootTracePath, O_RDONLY);
        if (previous >= 0) {
            const ssize_t n = read(previous, head, sizeof(head) - 1);
            rawClose(previous);
            head[n > 0 ? n : 0] = 0;
            gameRun = strstr(head, ps4_diag_config_game_marker) != nullptr;
        }
    }
    if (!gameRun && ps4_diag_config_game_stop_file && ps4_diag_config_game_stop_file[0]) {
        snprintf(previousPath, sizeof(previousPath), "%s/%s", g_dataDir, ps4_diag_config_game_stop_file);
        gameRun = pathExists(previousPath);
    }
    snprintf(previousPath, sizeof(previousPath), "%s/%s", g_dataDir,
             gameRun ? "boot-trace-game.log" : "boot-trace-prev.log");
    rename(g_bootTracePath, previousPath);
    clock_gettime(CLOCK_MONOTONIC, &g_traceStart);
    g_traceFd.store(rawOpen(g_bootTracePath, O_WRONLY | O_CREAT | O_TRUNC, 0666), std::memory_order_relaxed);
    g_traceState.store(2, std::memory_order_release);
    return g_traceFd.load(std::memory_order_relaxed);
}

// The trace's descriptor only if it is already open (inside the crash handler: nothing that waits).
int openTraceFd() {
    return g_traceState.load(std::memory_order_acquire) == 2 ? g_traceFd.load(std::memory_order_relaxed) : -1;
}

// ---------------------------------------------------------------------------------------------
// Readability probe for the crash handler's stack scan: the kernel copies 8 bytes into a pipe and
// reports EFAULT instead of faulting. The scan used to read past the top of a thread's stack, fault
// again inside the handler, and the system then ended the app with nothing in crash.log (HENkaku,
// Sonic).
int g_probePipe[2] = {-1, -1};

bool readable(const void* address) {
    if (g_probePipe[0] < 0)
        return false;
    if (write(g_probePipe[1], address, 8) != 8)
        return false;
    char drain[8];
    read(g_probePipe[0], drain, sizeof(drain));
    return true;
}

// Reads stack words from `start` while they are mapped: the first word, then each new 4 KiB page,
// is checked before it is read.
template <typename Visit>
void scanStack(const uint64_t* start, int maxWords, Visit visit) {
    for (int i = 0; i < maxWords; i++) {
        const uint64_t* p = start + i;
        if ((i == 0 || (reinterpret_cast<uintptr_t>(p) & 0xfff) == 0) && !readable(p))
            return;
        if (!visit(i, *p))
            return;
    }
}

// ---------------------------------------------------------------------------------------------
// The calling thread's own stack for the crash handler: a thread that runs out of stack faults on
// its guard page, and without another stack the handler can't run at all; the system then ends the
// app with error CE-34878-0 and nothing in crash.log. Every thread gets one (the runtime's
// pthread_create wrap; the main thread: earlyInit below), freed when the thread ends (a pthread key
// destructor): Devil May Cry 4 starts a thread per asynchronous read - 4,800 in 3 minutes - and the
// 64 KiB each kept filled the heap (rpcs3 v01.25).
//
// Below it a no-access guard page: an overflow of the signal stack faults instead of running into
// neighbouring heap memory (rpcs3: its access violation handler does Vulkan readbacks on this stack;
// at 64 KiB that overflow turned TTT2's pad state to garbage, v01.56/57).

// FreeBSD's stack_t (the OpenOrbis musl header has Linux's field order, which the kernel misreads)
struct BsdSignalStack {
    void* sp;
    size_t size;
    int flags;
};
extern "C" int ps4_sigaltstack(const BsdSignalStack* stack, BsdSignalStack* old) __asm__("sigaltstack");

constexpr int kSsDisable = 0x4;          // FreeBSD SS_DISABLE
constexpr size_t kSignalGuard = 16 * 1024;  // one PS4 page
std::atomic<size_t> g_signalStackSize{128 * 1024};
std::atomic<bool> g_signalStackStats{false};
std::atomic<size_t> g_signalStackDeepest{0};
pthread_key_t g_signalStackKey;
std::atomic<int> g_signalStackKeyState{0};  // 0 none, 1 creating, 2 ready

// The key's value: the block, its low bits the size in pages and whether it was zeroed (stats).
struct SignalStackBlock {
    void* block;
    size_t size;
    bool zeroed;
};
uintptr_t packBlock(void* block, size_t size, bool zeroed) {
    // the block is 16 KiB aligned: 14 free low bits; sizes are whole pages up to 8 MiB (9 bits)
    return reinterpret_cast<uintptr_t>(block) | ((size / kSignalGuard) << 1) | (zeroed ? 1 : 0);
}
SignalStackBlock unpackBlock(uintptr_t value) {
    return {reinterpret_cast<void*>(value & ~(kSignalGuard - 1)), ((value & (kSignalGuard - 1)) >> 1) * kSignalGuard,
            (value & 1) != 0};
}

void releaseSignalStack(void* value) {
    const SignalStackBlock b = unpackBlock(reinterpret_cast<uintptr_t>(value));
    const BsdSignalStack disable{nullptr, 0, kSsDisable};
    ps4_sigaltstack(&disable, nullptr);
    if (b.zeroed) {
        // how deep the handlers went: the stack was zeroed, it grows down from the top
        const auto* stack = static_cast<const uint8_t*>(b.block) + kSignalGuard;
        size_t untouched = 0;
        while (untouched < b.size && stack[untouched] == 0)
            untouched += 64;
        const size_t used = b.size - untouched;
        size_t deepest = g_signalStackDeepest.load(std::memory_order_relaxed);
        while (used > deepest && !g_signalStackDeepest.compare_exchange_weak(deepest, used)) {
        }
        if (used > deepest)
            ps4_boot_tracef("signal stack: deepest use so far %zu KiB of %zu", used / 1024, b.size / 1024);
    }
    ps4k_sceKernelMprotect(b.block, kSignalGuard, 3);  // read/write again before the heap takes it back
    free(b.block);
}

// ---------------------------------------------------------------------------------------------
// The crash handler

constexpr int kSigIll = 4, kSigAbrt = 6, kSigFpe = 8, kSigBus = 10, kSigSegv = 11;
constexpr int kSaSiginfo = 0x40, kSaOnstack = 0x1;
constexpr const char* kRegNames[] = {"rdi", "rsi", "rdx", "rcx", "r8",  "r9",  "rax", "rbx",
                                     "rbp", "r10", "r11", "r12", "r13", "r14", "r15"};

void setHandler(int sig, void (*handler)(int, siginfo_t*, void*), int flags) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    // The handler union is the first member; its member names differ between headers.
    memcpy(&action, &handler, sizeof(handler));
    action.sa_flags = flags;
    sigemptyset(&action.sa_mask);
    sigaction(sig, &action, nullptr);
}

void* crashTimer(void*) {
    sleep(15);
    endAfterCrash(126);
}

void crashHandler(int sig, siginfo_t* info, void* context) {
    static std::atomic<int> s_reporter{0};  // the thread writing the report
    static std::atomic<int> s_depth{0};
    const int self = pthread_getthreadid_np();
    const int depth = s_depth.fetch_add(1);
    const uint64_t* regs = regsOf(context);
    const uint64_t rip = regs[kRipIndex];
    const uint64_t rsp = regs[kRspIndex];
    const uint64_t address = *reinterpret_cast<const uint64_t*>(reinterpret_cast<char*>(info) + 24);
    const bool on = g_crashLogOn.load(std::memory_order_relaxed);

    // First, before anything that could fault: one line in the boot trace with where it happened
    // (static buffer, write() only). Crema 1.93 (Minecraft) crashed, the report itself faulted
    // before its first line, and the nested exit ended the app with nothing recorded.
    if (on) {
        static char early[200];
        const int traceFd = openTraceFd();
        const int n = snprintf(early, sizeof(early),
                               "fatal signal %d (thread %d, depth %d): rip 0x%llx%s elf 0x%llx, address 0x%llx\n", sig,
                               self, depth, static_cast<unsigned long long>(rip), isCode(rip) ? "" : " (not eboot code)",
                               static_cast<unsigned long long>(toElf(rip)), static_cast<unsigned long long>(address));
        if (traceFd >= 0 && n > 0)
            write(traceFd, early, static_cast<size_t>(n));
    }

    int reporter = 0;
    if (!s_reporter.compare_exchange_strong(reporter, self)) {
        // Crashed while reporting (typically the stack scan running off the end of a small thread
        // stack): end the process now. Returning re-ran the faulting read forever, leaving the
        // crashed thread spinning and the app hanging on a black screen. (The PS4 answers this
        // _exit with SIGSYS: the system's crash report, error CE-34878-0.)
        if (reporter == self)
            _exit(128 + sig);
        // Another thread crashed at the same time: the first one's report is the one to finish
        // (the ports' depth counter ended the process from here, cutting that report off). Its
        // timer ends the process; this is the fallback if it never gets that far.
        for (int i = 0; i < 20; i++)
            sleep(1);
        _exit(128 + sig);
    }

    if (!on) {
        if (ps4_crash_note)
            ps4_crash_note();
        endAfterCrash(128 + sig);
    }

    int fd = rawOpen(g_crashLogPath, O_WRONLY | O_CREAT | O_APPEND, 0666);
    const bool ownFile = fd >= 0;
    if (!ownFile && openTraceFd() >= 0) {
        // the report then goes to the boot trace, which is already open (Crema 1.73: the PS4's open
        // file limit was reached and the crash left no report; 1.69: opening files was failing)
        writeFormat(openTraceFd(), "signal %d: crash.log cannot be opened (errno %d), the report follows here\n", sig,
                    errno);
        fd = openTraceFd();
    }
    if (fd >= 0) {
        writeFormat(fd, "\n=== signal %d (code %d), fault address 0x%llx, thread %d\n", sig,
                    *reinterpret_cast<const int*>(reinterpret_cast<char*>(info) + 8),
                    static_cast<unsigned long long>(address), self);
        writeFormat(fd, "rip 0x%llx%s elf 0x%llx, rsp 0x%llx, text at %p\n", static_cast<unsigned long long>(rip),
                    isCode(rip) ? "" : " (not eboot code)", static_cast<unsigned long long>(toElf(rip)),
                    static_cast<unsigned long long>(rsp), static_cast<void*>(__text_start));
        for (int i = 0; i < 15; i++)
            writeFormat(fd, "%s 0x%llx%s", kRegNames[i], static_cast<unsigned long long>(regs[1 + i]),
                        i % 4 == 3 ? "\n" : "  ");
        if (address < rsp && rsp - address < 0x10000)
            writeAll(fd, "\nthe fault is just below the stack pointer: this thread ran out of stack\n");
        if (ps4_crash_guest_regs) {
            writeAll(fd, "\n");
            ps4_crash_guest_regs(fd);
        }
        if (ps4_crash_jit_info && !isCode(rip))
            ps4_crash_jit_info(fd, rip, 0);
        // Return addresses only (a call just before them): the full list of code-looking values
        // was mostly stale addresses and pointers into .rodata.
        writeAll(fd, "\ncode addresses on the stack (elf):\n");
        int found = 0;
        scanStack(reinterpret_cast<const uint64_t*>(rsp & ~7ull), 2048, [&](int i, uint64_t value) {
            if (isReturnAddress(value)) {
                writeFormat(fd, "  [rsp+0x%x] 0x%llx\n", i * 8, static_cast<unsigned long long>(toElf(value)));
                found++;
            }
            return found < 48;
        });
        if (ownFile)
            rawClose(fd);
    }
    if (openTraceFd() >= 0)
        writeAll(openTraceFd(), ownFile ? "crashed (see crash.log)\n" : "crashed\n");
    if (ps4_crash_note)
        ps4_crash_note();

    // What else to record takes locks and allocates, which a crash may have left held: a timer
    // thread ends the process (or restarts into the menu) if it doesn't finish.
    pthread_t timer;
    rawThread(&timer, crashTimer);
    // Where every other thread was (stacks, and the lock each one waits on), after the crash.
    ps4_dump_thread_stacks(g_crashLogPath, "every thread at the crash");
    // The app's view of the crash (Crema: the guest CPU state, stack trace and threads into log.txt;
    // HENkaku: the app that ran; M33/SMW: the PSP CPU; Resonance: the SH4; SRB2: game state and map).
    if (ps4_crash_guest_info) {
        ps4_crash_guest_info();
        if (openTraceFd() >= 0)
            writeAll(openTraceFd(), "the app's crash info written (ps4_crash_guest_info)\n");
    }

    // End the process here (or restart into the menu) rather than with the system's crash report:
    // on FW 13.52 that coredump of a homebrew app never finished and hung the console until a hard
    // power-off (DolphinPS4), and the report itself is encrypted.
    endAfterCrash(128 + sig);
}

// std::terminate (e.g. an uncaught exception): record what was thrown, then abort (the crash
// handler then writes the stacks).
[[noreturn]] void terminateHandler() {
    char message[512] = "std::terminate called";
    if (const std::type_info* type = abi::__cxa_current_exception_type()) {
        int status = 0;
        char* name = abi::__cxa_demangle(type->name(), nullptr, nullptr, &status);
        const char* what = "";
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
        try {
            if (std::exception_ptr current = std::current_exception())
                std::rethrow_exception(current);
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }
#endif  // (a -fno-exceptions build, like Dolphin's, gets the type only)
        snprintf(message, sizeof(message), "uncaught exception %s: %s", name ? name : type->name(), what);
    }
    if (g_crashLogOn.load(std::memory_order_relaxed)) {
        if (traceFd() >= 0)
            writeFormat(traceFd(), "%s\n", message);
        const int fd = rawOpen(g_crashLogPath, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd >= 0) {
            writeFormat(fd, "\n=== %s\n", message);
            rawClose(fd);
        } else if (traceFd() >= 0) {
            writeFormat(traceFd(), "crash.log cannot be opened (errno %d)\n", errno);
        }
    }
    abort();
}

__attribute__((constructor(101))) void earlyInit() {
    if (traceFd() >= 0)
        writeFormat(traceFd(), "static init (text at %p)\n", static_cast<void*>(__text_start));
    std::set_terminate(terminateHandler);
    if (pipe(g_probePipe) == 0) {
        fcntl(g_probePipe[0], F_SETFL, O_NONBLOCK);
        fcntl(g_probePipe[1], F_SETFL, O_NONBLOCK);
    }
    ps4_install_signal_stack();
    const int signals[] = {kSigIll, kSigAbrt, kSigFpe, kSigBus, kSigSegv};
    for (int sig : signals)
        setHandler(sig, crashHandler, kSaSiginfo | kSaOnstack);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Public API (ps4_kit_diag.h)

extern "C" const char* ps4_diag_data_dir() {
    resolveDataDir();
    return g_dataDir;
}

extern "C" int ps4_diag_data_dir_moved() {
    resolveDataDir();
    return g_dataMoved;
}

extern "C" const char* ps4_diag_crash_log_path() {
    resolveDataDir();
    return g_crashLogPath;
}

// The names HENkaku and Conker use (weak: a runtime file that defines its own wins).
extern "C" __attribute__((weak)) const char* ps4_data_dir() { return ps4_diag_data_dir(); }
extern "C" __attribute__((weak)) int ps4_data_dir_moved() { return ps4_diag_data_dir_moved(); }
extern "C" __attribute__((weak)) const char* ps4_crash_log_path() { return ps4_diag_crash_log_path(); }

extern "C" void ps4_diag_set_crash_log(int on) { g_crashLogOn.store(on != 0); }
extern "C" int ps4_diag_crash_log_enabled() { return g_crashLogOn.load() ? 1 : 0; }
extern "C" void ps4_diag_set_boot_trace(int on) { g_bootTraceOn.store(on != 0); }
extern "C" int ps4_diag_boot_trace_enabled() { return g_bootTraceOn.load() ? 1 : 0; }
extern "C" void ps4_diag_set_open_tracking(int on) { g_openTrackingOn.store(on != 0); }

extern "C" void ps4_diag_set_signal_stack_size(size_t bytes) {
    // whole pages, 32 KiB to 8 MiB (the size travels in the key value's low bits)
    bytes = (bytes + kSignalGuard - 1) & ~(kSignalGuard - 1);
    if (bytes < 2 * kSignalGuard)
        bytes = 2 * kSignalGuard;
    if (bytes > 511 * kSignalGuard)
        bytes = 511 * kSignalGuard;
    g_signalStackSize.store(bytes);
}

extern "C" void ps4_diag_set_signal_stack_stats(int on) { g_signalStackStats.store(on != 0); }

extern "C" void ps4_install_signal_stack() {
    const size_t size = g_signalStackSize.load(std::memory_order_relaxed);
    const bool zeroed = g_signalStackStats.load(std::memory_order_relaxed);
    void* block = memalign(kSignalGuard, kSignalGuard + size);
    if (!block)
        return;
    if (zeroed)
        memset(block, 0, kSignalGuard + size);
    ps4k_sceKernelMprotect(block, kSignalGuard, 0);  // the guard: no access
    const BsdSignalStack signalStack{static_cast<uint8_t*>(block) + kSignalGuard, size, 0};
    if (ps4_sigaltstack(&signalStack, nullptr) != 0) {
        ps4k_sceKernelMprotect(block, kSignalGuard, 3);
        free(block);
        return;
    }
    int state = 0;
    if (g_signalStackKeyState.compare_exchange_strong(state, 1)) {
        pthread_key_create(&g_signalStackKey, releaseSignalStack);
        g_signalStackKeyState.store(2, std::memory_order_release);
    }
    while (g_signalStackKeyState.load(std::memory_order_acquire) != 2)
        sched_yield();
    // a second call in the same thread: the earlier block goes (the system no longer uses it)
    if (void* earlier = pthread_getspecific(g_signalStackKey)) {
        const SignalStackBlock b = unpackBlock(reinterpret_cast<uintptr_t>(earlier));
        ps4k_sceKernelMprotect(b.block, kSignalGuard, 3);
        free(b.block);
    }
    pthread_setspecific(g_signalStackKey, reinterpret_cast<void*>(packBlock(block, size, zeroed)));
}

// Appends a line to <data dir>/boot-trace.log (startup milestones), prefixed with the seconds since
// the trace was opened.
extern "C" void ps4_boot_trace(const char* stage) {
    if (!g_bootTraceOn.load(std::memory_order_relaxed))
        return;
    const int fd = traceFd();
    if (fd < 0 || !stage)
        return;
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const double seconds = static_cast<double>(now.tv_sec - g_traceStart.tv_sec) +
                           static_cast<double>(now.tv_nsec - g_traceStart.tv_nsec) / 1e9;
    // One write per line, so lines from different threads don't interleave. Room for a monitor's
    // long lines; a cut line still ends with its newline (a truncated monitor line used to run into
    // the next one).
    char line[2048];
    const size_t length = strlen(stage);
    const bool newline = length == 0 || stage[length - 1] != '\n';
    const int written = snprintf(line, sizeof(line), "[%8.3f] %s%s", seconds, stage, newline ? "\n" : "");
    if (written >= static_cast<int>(sizeof(line)))
        line[sizeof(line) - 2] = '\n';
    writeAll(fd, line);
}

extern "C" void ps4_boot_tracef(const char* fmt, ...) {
    if (!g_bootTraceOn.load(std::memory_order_relaxed))
        return;
    char text[1900];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    ps4_boot_trace(text);
}

// "<what>: callers 0x.. 0x.." in the boot trace: return addresses on the calling thread's stack.
extern "C" __attribute__((noinline)) void ps4_trace_callers(const char* what) {
    // Read from the stack pointer itself: walking past a local variable is undefined behaviour the
    // optimizer is free to drop (an earlier version printed nothing).
    const uint64_t* stack;
    asm volatile("mov %%rsp, %0" : "=r"(stack));
    char line[320];
    int len = snprintf(line, sizeof(line), "%s: callers", what);
    int found = 0;
    scanStack(stack, 512, [&](int, uint64_t value) {
        if (isReturnAddress(value)) {
            len += snprintf(line + len, sizeof(line) - len, " 0x%llx", static_cast<unsigned long long>(toElf(value)));
            found++;
        }
        return found < 10 && len < 280;
    });
    ps4_boot_trace(line);
}

// ---------------------------------------------------------------------------------------------
// Every open() and close() of the app's own code (its libraries, Mesa, libc++, and the C library's
// fopen, which calls open; linker --wrap): the path, when, and the return addresses that opened it,
// so files left open can be named with their opener. The PS4 refuses new files at ~250 distinct
// opens, and Tekken Tag Tournament 2 leaked one every ~7 s until its own file loads failed and it
// stopped itself (rpcs3/Crema 1.75). Sony's libraries open files directly through the kernel and
// aren't seen here.
namespace {

constexpr int kTrackedFds = 1024;  // descriptors are the lowest free number: far below this
struct OpenRecord {
    char path[104];
    uint32_t callers[5];  // elf addresses of code on the stack at the open, innermost first
    float seconds;
    bool used;
};
OpenRecord g_openRecords[kTrackedFds];
uint64_t g_trackStart = 0;

float secondsSinceStart() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint64_t ns = static_cast<uint64_t>(now.tv_sec) * 1000000000ull + now.tv_nsec;
    if (g_trackStart == 0)
        g_trackStart = ns;
    return static_cast<float>((ns - g_trackStart) / 1e9);
}

}  // namespace

extern "C" __attribute__((noinline)) void ps4_diag_note_open(int fd, const char* path) {
    if (fd < 0 || fd >= kTrackedFds || !g_openTrackingOn.load(std::memory_order_relaxed))
        return;
    OpenRecord& record = g_openRecords[fd];
    // the end of the path is the telling part
    const size_t length = path ? strlen(path) : 0;
    const char* tail = length >= sizeof(record.path) ? path + length - (sizeof(record.path) - 1) : path;
    memset(record.callers, 0, sizeof(record.callers));
    int found = 0;
    // (bounded and probed: 400 words above the frame ran past the top of a thread that opens a file
    // first thing)
    scanStack(static_cast<const uint64_t*>(__builtin_frame_address(0)), 400, [&](int, uint64_t value) {
        if (isReturnAddress(value))
            record.callers[found++] = static_cast<uint32_t>(toElf(value));
        return found < 5;
    });
    snprintf(record.path, sizeof(record.path), "%s", tail ? tail : "(null)");
    record.seconds = secondsSinceStart();
    record.used = true;
}

extern "C" void ps4_diag_note_close(int fd) {
    if (fd >= 0 && fd < kTrackedFds)
        g_openRecords[fd].used = false;
}

// The wraps for a build without a file layer of its own (weak: the runtime's __wrap_open - the
// network paths, relative paths - wins, and calls ps4_diag_note_open/close itself).
extern "C" __attribute__((weak, noinline)) int __wrap_open(const char* path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    const int mode = va_arg(args, int);  // always passed on: harmless when the call had none
    va_end(args);
    if (!__real_open) {
        errno = ENOSYS;
        return -1;
    }
    const int fd = __real_open(path, flags, mode);
    ps4_diag_note_open(fd, path);
    return fd;
}

extern "C" __attribute__((weak)) int __wrap_close(int fd) {
    // (first: the number may be handed out again as soon as it's closed)
    ps4_diag_note_close(fd);
    return __real_close ? __real_close(fd) : close(fd);
}

// The files open now, except the descriptors in `skip` (a byte per descriptor, nonzero to skip:
// Crema's FileStreams, which it reports itself): grouped by folder and opener, then the newest.
extern "C" void ps4_open_calls_report(const unsigned char* skip) {
    struct Group {
        char folder[104];
        uint32_t caller;
        int count;
    };
    static Group groups[64];
    int groupCount = 0, total = 0;
    const float now = secondsSinceStart();
    for (int fd = 0; fd < kTrackedFds; fd++) {
        const OpenRecord& record = g_openRecords[fd];
        if (!record.used || (skip && skip[fd]))
            continue;
        total++;
        char folder[104];
        snprintf(folder, sizeof(folder), "%s", record.path);
        if (char* slash = strrchr(folder, '/'))
            *slash = 0;
        // the opener: the first caller outside the C library's own open paths is not known here,
        // so the group key is the folder and the outermost of the first three callers
        const uint32_t caller = record.callers[2] ? record.callers[2] : record.callers[0];
        int g = 0;
        while (g < groupCount && (groups[g].caller != caller || strcmp(groups[g].folder, folder) != 0))
            g++;
        if (g == groupCount) {
            if (groupCount == 64)
                continue;
            snprintf(groups[g].folder, sizeof(groups[g].folder), "%s", folder);
            groups[g].caller = caller;
            groups[g].count = 0;
            groupCount++;
        }
        groups[g].count++;
    }
    char line[300];
    snprintf(line, sizeof(line), "open() files still open: %d in %d groups%s", total, groupCount,
             g_openTrackingOn.load() ? "" : " (tracking is off: opens since then are missing)");
    ps4_boot_trace(line);
    for (int shown = 0; shown < 12; shown++) {
        int best = -1;
        for (int g = 0; g < groupCount; g++)
            if (groups[g].count > 0 && (best < 0 || groups[g].count > groups[best].count))
                best = g;
        if (best < 0)
            break;
        snprintf(line, sizeof(line), "  %dx %s (opener elf 0x%x)", groups[best].count, groups[best].folder,
                 groups[best].caller);
        ps4_boot_trace(line);
        groups[best].count = -groups[best].count;  // shown
    }
    // the six opened last that are still open
    int newest[6] = {-1, -1, -1, -1, -1, -1};
    for (int fd = 0; fd < kTrackedFds; fd++) {
        const OpenRecord& record = g_openRecords[fd];
        if (!record.used || (skip && skip[fd]))
            continue;
        // replace the oldest kept one (an empty slot first) if this one is newer
        int oldest = 0;
        for (int i = 1; i < 6 && newest[oldest] >= 0; i++)
            if (newest[i] < 0 || g_openRecords[newest[i]].seconds < g_openRecords[newest[oldest]].seconds)
                oldest = i;
        if (newest[oldest] < 0 || g_openRecords[newest[oldest]].seconds < record.seconds)
            newest[oldest] = fd;
    }
    for (int i = 0; i < 6; i++) {
        if (newest[i] < 0)
            continue;
        const OpenRecord& record = g_openRecords[newest[i]];
        snprintf(line, sizeof(line), "  newest: fd %d at %.0f s (%.0f s ago) %s, callers elf 0x%x 0x%x 0x%x 0x%x 0x%x",
                 newest[i], record.seconds, now - record.seconds, record.path, record.callers[0], record.callers[1],
                 record.callers[2], record.callers[3], record.callers[4]);
        ps4_boot_trace(line);
    }
}
