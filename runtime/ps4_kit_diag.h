// ps4-kit diagnostics: crash log, boot trace, open-file tracking, hang dumps, CPU-busy report and
// the three profilers (ps4_crashlog.cpp, ps4_sampler.cpp, ps4_profile.cpp).
//
// Every diagnostic has a run-time switch the app's settings can flip (Diagnostics menu): the crash
// log, boot trace, open-file tracking and thread dumps start on (they cost nothing until something
// goes wrong, and the first crash of a new build is the one that matters); the profilers start off
// (each interrupts the app's threads with signals).
//
// Files go to the app's folder, ps4_diag_data_dir() (see "Configuration" below):
//   boot-trace.log       this run's milestones (ps4_boot_trace); the last run's is kept as
//                        boot-trace-prev.log, or boot-trace-game.log after a game run (see
//                        ps4_diag_config_game_marker)
//   crash.log            every crash (appended): signal, registers, code addresses on the stack,
//                        then every watched thread's stack (ps4_dump_thread_stacks)
//   samples.log          the 1 kHz sampler (ps4_sampler_set_enabled), every 10 s
//   profile.log          the hot-spot profiler (ps4_hot_profiler_set_enabled), every 10 s
//   profiles/profile_N.txt   the burst profiler (ps4_profile_start)
//   jitcode.bin, mem1.bin, hotfuncs.bin   code dumps while the sampler runs (JIT ports only)
// Addresses in them are ELF virtual addresses (relative to .text): symbolize with
// `llvm-symbolizer --obj=<the build's elf> 0x...`, or scripts/crashlog.sh and scripts/profile.py.
//
// Signals used (FreeBSD numbers): SIGILL/ABRT/FPE/BUS/SEGV (crash handler), SIGPROF 27 (sampler),
// SIGVTALRM 26 (burst profiler), SIGUSR1 30 (thread dumps, hot-spot profiler), SIGUSR2 31 (CPU-busy
// report). An app that needs one of these for itself must not use that diagnostic.

#pragma once

#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------
// Configuration: optional definitions in the port, read before main() (the heap may write the boot
// trace before any constructor of the app has run, so these can't be set by a function call).
// Define them in one C++ file of the port, e.g.:
//   PS4_DIAG_DATA_DIR("/data/Crema");
//   PS4_DIAG_OLD_DATA_DIR("/data/CemuPS4");                 // renamed once, on the first start
//   PS4_DIAG_MIGRATE_FILES("launcher.ini", "settings.xml"); // paths inside them follow the rename
//   PS4_DIAG_GAME_MARKER("] game: ");                       // see boot-trace-game.log above
// Without PS4_DIAG_DATA_DIR the folder is the build's -DPS4_KIT_DATA_DIR="..." or /data/ps4-kit.
#ifdef __cplusplus
#define PS4_DIAG_CONFIG_ extern "C" __attribute__((used))
#else
#define PS4_DIAG_CONFIG_ __attribute__((used))
#endif
#define PS4_DIAG_DATA_DIR(path) PS4_DIAG_CONFIG_ const char ps4_diag_config_data_dir[] = path
#define PS4_DIAG_OLD_DATA_DIR(path) PS4_DIAG_CONFIG_ const char ps4_diag_config_old_data_dir[] = path
#define PS4_DIAG_MIGRATE_FILES(...) \
    PS4_DIAG_CONFIG_ const char* const ps4_diag_config_migrate_files[] = {__VA_ARGS__, 0}
#define PS4_DIAG_GAME_MARKER(text) PS4_DIAG_CONFIG_ const char ps4_diag_config_game_marker[] = text
// (the boot trace of a run that left this file in the data folder is kept as boot-trace-game.log
// too: an emulator's "the game stopped, back to the menu" note)
#define PS4_DIAG_GAME_STOP_FILE(name) PS4_DIAG_CONFIG_ const char ps4_diag_config_game_stop_file[] = name

// ---------------------------------------------------------------------------------------------
// The app's folder and the crash log (ps4_crashlog.cpp)

const char* ps4_diag_data_dir(void);       // e.g. "/data/Crema"; created on first use
int ps4_diag_data_dir_moved(void);         // 1: this start renamed the old folder; -1: it couldn't
const char* ps4_diag_crash_log_path(void); // <data dir>/crash.log

// Crash log on/off (default on). Off: a crash writes nothing, but the app still ends itself (or
// restarts into its menu, ps4_crash_restart) instead of leaving it to the system: the system's
// core dump of a homebrew app hung FW 13.52 consoles until a hard power-off (DolphinPS4).
void ps4_diag_set_crash_log(int on);
int ps4_diag_crash_log_enabled(void);

// Boot trace on/off (default on). Off: ps4_boot_trace() lines are dropped (the file of this run is
// already open by the time settings are read; it just stops growing).
void ps4_diag_set_boot_trace(int on);
int ps4_diag_boot_trace_enabled(void);
// One line in boot-trace.log, "[seconds since start] text"; one write per line (threads don't
// interleave), lines up to 2 KiB.
void ps4_boot_trace(const char* stage);
void ps4_boot_tracef(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// "<what>: callers 0x.. 0x.." in the boot trace: the return addresses on the calling thread's stack
// (a cheap "who calls this" without frame pointers; now and then a stale one).
void ps4_trace_callers(const char* what);

// The per-thread stack the crash handler runs on (also every SA_ONSTACK handler: fastmem, RPCS3's
// access violation handler). Call ps4_install_signal_stack() first thing in every thread (the
// runtime's pthread_create wrap; the main thread gets one at static init); it is freed when the
// thread ends. Without it a thread that overflows its stack dies with CE-34878-0 and no report.
void ps4_install_signal_stack(void);
// Its size (default 128 KiB, plus a no-access guard page): set before threads start. 512 KiB for a
// handler that does real work (rpcs3: Vulkan readbacks inside its access violation handler).
void ps4_diag_set_signal_stack_size(size_t bytes);
// Off by default: zero each signal stack and log the deepest use seen when a thread ends.
void ps4_diag_set_signal_stack_stats(int on);

// Open-file tracking (default on): every open()/close() of the app's own code with its path, time
// and callers, so a descriptor leak names its opener (the PS4 refuses new files at ~250 open).
void ps4_diag_set_open_tracking(int on);
// For the file layer's own __wrap_open/__wrap_close (if it has one; else the kit's weak ones are
// used): report each successful open and each close (before the real close).
void ps4_diag_note_open(int fd, const char* path);
void ps4_diag_note_close(int fd);
// The open files into the boot trace, grouped by folder and opener, then the newest six. `skip`: a
// byte per descriptor (nonzero = leave out, e.g. ones the app reports itself), or null.
void ps4_open_calls_report(const unsigned char* skip);

// ---------------------------------------------------------------------------------------------
// Thread watch, hang dumps, CPU-busy report (ps4_sampler.cpp)

// Registers the calling thread for thread dumps and the busy report (the runtime's thread start
// does it for every thread; a later call renames). The slot is freed when the thread ends.
void ps4_watch_thread(const char* name);
// Every watched thread's instruction pointer and return addresses (and the lock it waits on) into
// `path` (appended), headed by `reason`. For the app's watchdog; also called by the crash handler.
void ps4_dump_thread_stacks(const char* path, const char* reason);
// Thread dumps on/off (default on): off, ps4_dump_thread_stacks writes only its heading.
void ps4_diag_set_thread_dumps(int on);
int ps4_diag_thread_dumps_enabled(void);

// Busy threads report their own CPU time (the PS4 refuses another thread's CPU clock): call this in
// a thread's loop, it reads the clock at most every ~10 ms.
void ps4_thread_cpu_tick(void);
// "name N%, name N%" of the watched threads at or above min_percent since the previous call,
// busiest first (threads that don't tick are asked by signal). One caller (the app's monitor).
void ps4_thread_busy_report(char* out, size_t size, int min_percent);
// Two fixed slots (0, 1) of self-sampled CPU time, for an app's own monitor lines.
void ps4_thread_cpu_sample(int slot);
double ps4_thread_cpu_seconds(int slot);
// App-defined wait-time counters (ns), slots 0-15: add from anywhere, read totals from the monitor.
void ps4_wait_add(int slot, long long ns);
long long ps4_wait_total(int slot);

// ---------------------------------------------------------------------------------------------
// The 1 kHz sampler (ps4_sampler.cpp): SIGPROF to each registered thread every millisecond.
// Off by default; costs nothing until first switched on (buffers are allocated then).
void ps4_sampler_register_thread(const char* name);  // the threads worth sampling (up to 16)
void ps4_sampler_set_enabled(int on);                // the Profiler switch: start / pause / resume
int ps4_sampler_enabled(void);
void ps4_sampler_start(void);                        // = ps4_sampler_set_enabled(1)
// Guest RAM to dump with the JIT code every 60 s (mem1.bin), for offline disassembly.
void ps4_sampler_set_guest_ram(void* base, uint32_t size);
// jitcode.bin / mem1.bin / hotfuncs.bin dumps while the sampler runs (default on; only written
// when there is JIT code in the samples or guest RAM was given).
void ps4_sampler_set_code_dumps(int on);

// The hot-spot profiler (ps4_sampler.cpp): every 20 ms a look at each watched thread that was
// running at its last look, blocked threads once a second: far fewer interruptions than 1 kHz.
// profile.log every 10 s. Off by default.
void ps4_hot_profiler_set_enabled(int on);
int ps4_hot_profiler_enabled(void);

// ---------------------------------------------------------------------------------------------
// The burst profiler (ps4_profile.cpp): a profile of a few seconds of every (or the chosen) thread,
// e.g. when the frame rate drops; profiles/profile_N.txt. Off by default.
void ps4_profile_thread_begin(void);  // every thread at its start (the runtime's thread start)
void ps4_profile_set_enabled(int on);
int ps4_profile_enabled(void);
// Threads to sample, by the start of their names (null/0: every registered thread). A thread that
// waits on the GPU or display should not be interrupted (SMW froze after ~40 s of sampling).
void ps4_profile_set_threads(const char* const* prefixes, int count);
bool ps4_profile_start(double seconds, const char* reason);  // false while one runs, or when off
int ps4_profile_running(void);

// ---------------------------------------------------------------------------------------------
// Hooks: optional functions the port defines to add its own lines (the kit calls them only if they
// exist). "Lock-free" ones run inside the crash handler before anything else: no locks, no heap.
#ifndef PS4_KIT_DIAG_INTERNAL
void ps4_crash_guest_regs(int fd);   // lock-free: the guest CPU's registers, into crash.log
// lock-free: which translated guest function a host address is in (brief: one line)
void ps4_crash_jit_info(int fd, uint64_t rip, int brief);
void ps4_crash_guest_info(void);     // after the thread dump, may lock and allocate (timer-guarded)
void ps4_crash_note(void);           // leave a note for the app's menu after a crash
void ps4_crash_restart(void);        // restart into the app's menu instead of exiting (noreturn)
void ps4_recompiler_status(int fd);  // at the end of a thread dump (may lock)
void ps4_hang_guest_threads(int fd); // at the end of a thread dump (may lock)
// The sampler's JIT samples: the guest function a host address belongs to, and its code for
// hotfuncs.bin (records: u32 'HFNC', guest addr, guest bytes, host bytes, samples; u64 host addr;
// the guest code; the host code; scripts/hotfuncs.py reads them)
int ps4_jit_owner(uint64_t rip, uint32_t* guest_start, uint64_t* host_start);
void ps4_jit_dump_function(int fd, uint32_t guest_start, uint64_t host_start, uint32_t samples);
const char* ps4_profile_thread_description(int tid);  // a thread's full name, for the burst profiler
void ps4_heap_private_arena(const char* name);        // the runtime's: called by ps4_watch_thread
#endif

// Compatibility globals (Dolphin's Memmap.cpp sets them directly): = ps4_sampler_set_guest_ram.
extern void* ps4_profile_guest_ram;
extern uint32_t ps4_profile_guest_ram_size;

#ifdef __cplusplus
}
#endif

// ---------------------------------------------------------------------------------------------
// Internal to the kit's diagnostics files: code-address tests safe inside signal handlers.
#ifdef PS4_KIT_DIAG_INTERNAL
extern "C" char __text_start[];     // the OpenOrbis link.x: the start of .text
extern "C" char __eh_frame_start[]; // right after .text and .rodata: everything below it is mapped

namespace ps4diag {

// FreeBSD values (the PS4 kernel's); the OpenOrbis musl headers may carry Linux ones.
constexpr int kMcontextOffset = 0x40;  // ucontext -> mcontext, measured on hardware
constexpr int kRdiIndex = 1, kRsiIndex = 2, kRdxIndex = 3, kR10Index = 10, kRbpIndex = 9;
constexpr int kRipIndex = 20, kRspIndex = 23;  // in FreeBSD's mcontext, after mc_onstack
constexpr uint64_t kSystemLibs = 0x800000000ULL, kSystemEnd = 0x1000000000ULL;  // Sony's modules

inline const uint64_t* regsOf(void* context) {
    return reinterpret_cast<const uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
}
inline uintptr_t textStart() { return reinterpret_cast<uintptr_t>(__text_start); }
// The eboot's code (and read-only data) ends where .eh_frame starts. The ports guessed a window
// instead - 64 MiB, then 128 MiB for rpcs3's 67 MiB of code, 1 GiB for the recompiled games - and
// the 1 GiB one read unmapped memory (SSBU v00.12: a float on the stack, 0x3f800000, looked like
// code and its "call" bytes were read).
inline uintptr_t textEnd() { return reinterpret_cast<uintptr_t>(__eh_frame_start); }
inline bool isCode(uint64_t a) { return a >= textStart() && a < textEnd(); }
inline uint64_t toElf(uint64_t a) { return a - textStart(); }
inline bool isSystem(uint64_t a) { return a >= kSystemLibs && a < kSystemEnd; }

// The length of an `FF /2` (indirect call) instruction from its ModRM (and SIB) byte.
inline int indirectCallLength(uint8_t modrm, uint8_t sib) {
    const int mod = modrm >> 6, rm = modrm & 7;
    if (mod == 3)
        return 2;
    int length = 2;
    if (rm == 4)
        length += 1 + ((mod == 0 && (sib & 7) == 5) ? 4 : 0);
    else if (mod == 0 && rm == 5)
        length += 4;  // [rip + disp32]
    return length + (mod == 1 ? 1 : mod == 2 ? 4 : 0);
}

// A return address into the eboot: inside its code, right after a call instruction. Stack scans
// find every value that looks like code - stale return addresses, pointers into .rodata, floats -
// and named the wrong callers (SSBU v00.13, M33's sampler); checking the call before it removes
// most. Reads at most 7 bytes below `a`, all inside the eboot's mapped image.
inline bool isReturnAddress(uint64_t a) {
    if (a < textStart() + 8 || a >= textEnd())
        return false;
    const auto* r = reinterpret_cast<const uint8_t*>(a);
    if (r[-5] == 0xE8)
        return true;  // call rel32
    for (int length = 2; length <= 7; length++) {
        const uint8_t* p = r - length;
        if (p[0] == 0xFF && ((p[1] >> 3) & 7) == 2 && indirectCallLength(p[1], p[2]) == length)
            return true;
    }
    return false;
}

// In a system module, waiting in the kernel: right after a `syscall` (0f 05), or on it - a blocking
// call a signal interrupted is restarted by putting the thread back on its `syscall` (libkernel's
// _umtx_op stub at +0x371a: why libkernel 0x800003710 tops so many profiles; it is a wait, not work).
inline bool inSyscall(uint64_t rip) {
    if (!isSystem(rip))
        return false;
    const auto* code = reinterpret_cast<const uint8_t*>(rip);
    if ((rip & 0xFFF) >= 2 && code[-2] == 0x0F && code[-1] == 0x05)
        return true;
    return (rip & 0xFFF) <= 0xFFE && code[0] == 0x0F && code[1] == 0x05;
}

}  // namespace ps4diag
#endif  // PS4_KIT_DIAG_INTERNAL
