// ps4-kit: the runtime core's public API (ps4_runtime.cpp, ps4_dlmalloc.c). Plain C, callable from
// C and C++.
//
// Most of the runtime needs no call at all: it works through the linker's --wrap (the malloc
// family, mmap, clock_gettime, pthread_once, pthread_create, munmap/mprotect; the list is in
// docs/merge-notes/runtime-core.md) and through the libc malloc-replacement table in ps4_crt1.S.
// What is here: the switches whose right setting depends on the app, and the diagnostics an app's
// monitor or menu may show. Each switch documents when it must be set; the defaults are what the
// newest ports settled on.

#ifndef PS4_KIT_H
#define PS4_KIT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Heap: set before the first malloc ---------------------------------------------------------
//
// The heap is created by the first allocation, which happens in a static constructor, before
// main(). Its settings therefore come from a hook the app may define, called once at that moment:
//
//   void ps4_kit_configure_heap(ps4_kit_heap_options* o) { o->system_pool_max = 512u << 20; }
//
// It runs before any static constructor of the app: it may only fill in fields (no allocation, no
// C++ objects, no system calls).

enum {
    PS4_KIT_HEAP_SYSTEM_FIRST = 0,  // system flexible pool, then regular flexible, then direct
    PS4_KIT_HEAP_DIRECT_FIRST = 1,  // direct memory first (apps whose eboot fills flexible memory)
};

typedef struct ps4_kit_heap_options {
    // Prefix of every log line ("[tag] heap: ...") and of the kernel's names for the mappings
    // ("tag heap", "tag mmap", "tag code"). Default "ps4".
    const char* log_tag;
    // Where the main heap comes from. Default PS4_KIT_HEAP_SYSTEM_FIRST.
    int source_order;
    // Largest main heap tried in the system flexible pool. Default 256 MiB: with 512 MiB, std::mutex
    // could no longer get its kernel object two minutes into a game ("mutex lock failed: Out of
    // memory"); those objects, the system libc heap and Sony's modules use the same pool. The
    // overflow heaps (direct memory) take what does not fit.
    size_t system_pool_max;
    // PS4_KIT_HEAP_DIRECT_FIRST: largest main heap tried in direct memory. Default 1 GiB.
    size_t direct_max;
    // Regular flexible memory left to the system (Piglet, OpenGL ES) when the heap has to come from
    // there. Default 320 MiB.
    size_t regular_reserve;
    // Nonzero: the main heap's exact base address (16 KiB aligned, a guard page goes in front). For
    // apps that resolve 32-bit guest pointers by their low bits and need the heap's low 32 bits
    // clear of other ranges. Falls back to the usual placement when the range is taken. Default 0.
    uintptr_t fixed_address;
    // Private per-thread arenas (see ps4_kit_set_arena_threads), carved out of the main heap.
    // Defaults 10 MiB x 20 and, for the big list, 32 MiB x 10.
    size_t arena_size;
    int max_arenas;
    size_t big_arena_size;
    int max_big_arenas;
    // Overflow heaps: direct memory mapped when the main heap is full, given back by ps4_heap_trim
    // once empty. Default 6 heaps of up to 512 MiB, each leaving overflow_direct_reserve (default
    // 192 MiB) of direct memory to the app (GPU memory, an emulated machine's RAM).
    int max_overflow_heaps;
    size_t overflow_direct_reserve;
    // Automatic arenas: a thread that has had to wait for a heap lock auto_arena_waits times gets a
    // private arena of auto_arena_size, whatever its name, up to max_auto_arenas of them (an exited
    // thread's arena is reused first). Six threads sharing the one heap made a fifth of one thread's
    // malloc rate (kit-test v00.02). Defaults 32 waits, 4 MiB x 12; auto_arena_waits 0 turns it off.
    int auto_arena_waits;
    size_t auto_arena_size;
    int max_auto_arenas;
} ps4_kit_heap_options;

// Optional, defined by the app (weak here).
void ps4_kit_configure_heap(ps4_kit_heap_options* options) __attribute__((weak));

// --- Heap: per-thread arenas -------------------------------------------------------------------
//
// One locked heap made an emulator's busy threads contend (Ultimate Spider-Man's video thread spent
// ~9% of its time waiting; Cemu's recompiler took seconds per function). Threads named here get a
// private dlmalloc arena when they name themselves (ps4_sampler.cpp: ps4_watch_thread ->
// ps4_heap_private_arena); frees from any thread return to the right arena. No thread is listed by
// default. An entry ending in '*' matches by prefix ("PPU[*"). The strings are kept, not copied
// (string literals), at most 64 per list. Set before the threads start.
void ps4_kit_set_arena_threads(const char* const* names, int count);
// The same for allocation-heavy compiler threads (LLVM, shader compilers): big arenas.
void ps4_kit_set_big_arena_threads(const char* const* names, int count);
// Called when a thread names itself; gives it an arena if the name is listed.
void ps4_heap_private_arena(const char* name);
// Gives the calling thread an arena now, whatever its name (big: 1 for a big arena). 1 if it has one.
int ps4_kit_private_arena(int big);
// Automatic arenas at run time: the lock waits after which a thread gets one (0: none from now on;
// threads that already have one keep it). The start value is auto_arena_waits.
void ps4_kit_set_auto_arena_waits(int waits);
// Automatic arenas made so far (reused ones not counted again).
int ps4_kit_auto_arena_count(void);

// --- Heap: diagnostics -------------------------------------------------------------------------

const char* ps4rt_heap_source(void);       // "system flexible memory", "direct memory", ...
size_t ps4rt_heap_size(void);              // the main heap's size in bytes
size_t ps4rt_heap_footprint(void);         // how far the main heap has been used (upper bound)
size_t ps4_heap_footprint(void);           // the same, main heap plus the overflow heaps
// "heap N of M MiB filled (+ overflow O of P MiB)" for a periodic report line.
void ps4rt_heap_report(char* out, size_t size);
// MiB in use in all heaps (within ~1 MiB per thread); *overflow_heaps (may be NULL): mapped now.
size_t ps4_heap_in_use_mib(int* overflow_heaps);
// Call every ~5 s (a monitor thread): overflow heaps empty for two calls give their direct memory
// back; every 12th call also writes the heap profile when it is on.
void ps4_heap_trim(void);
// Sampling heap profiler (about one allocation per 256 KiB, every one of 256 KiB and more): the
// call sites holding the most live memory, into the boot trace. Off by default; its tables (~3.5 MiB)
// are mapped when it is first turned on.
void ps4_kit_set_heap_profiler(int on);
void ps4_heap_profile_report(void);

// --- Executable memory and mmap ----------------------------------------------------------------

// Executable mappings from the system flexible pool when it runs code (default 1). 0 keeps them
// out of it. Read once, at the first executable mmap. (DolphinPS4's ps4.ini sets it directly.)
extern int ps4_jit_in_system_pool;
void ps4_kit_set_jit_in_system_pool(int on);
// Finds the way this console lets homebrew run generated code (mmap does it on first use; an app
// may call it at startup to log it early). Returns the source (0..5) or -1 when none runs code.
int ps4_exec_probe(void);
const char* ps4_kit_exec_source(void);  // its name, "not probed yet" before the first probe
// Inaccessible anonymous mappings of 1 MiB and more only reserve address space, with no memory
// behind them (QEMU reserves guest RAM PROT_NONE, then maps MAP_FIXED inside). Default 0: such a
// mapping is backed, so a later mprotect to read/write works.
void ps4_kit_set_mmap_reserve_prot_none(int on);
// munmap/mprotect calls that would unmap the eboot or change its read-only/code segments are
// refused and logged with their caller (default 1).
void ps4_kit_set_image_guard(int on);

// --- Threads -----------------------------------------------------------------------------------

// The cores pthread_create gives new threads (a PS4 thread otherwise inherits its creator's).
// Default: the main thread's cores at the first allocation (0x7F when the app has the 7th core).
extern uint64_t ps4_new_thread_cores;
void ps4_kit_set_new_thread_cores(uint64_t mask);
// Every new thread gets at least this much stack (default 2 MiB).
void ps4_kit_set_min_thread_stack(size_t bytes);
// Raises the calling thread above the default priority (256 highest .. 767 lowest, 700 default):
// for threads that never spin on another thread's locks (GPU submission, emulated CPUs).
void ps4_boost_thread(void);
void ps4_kit_set_boost_priority(int priority);  // default 650 (audio stays higher at 400)
// Every kit-started thread's outermost app frame: a backtrace may stop here (unwinding further
// steps into system code the eboot's unwind tables don't cover).
void* ps4_thread_entry(void* raw);

// --- Time --------------------------------------------------------------------------------------

// CLOCK_MONOTONIC reads served from the TSC; counted only when turned on (default 0: a counter
// written by every thread costs a shared cache line on each read).
extern unsigned long long ps4_clock_reads;
void ps4_kit_set_count_clock_reads(int on);
// Diagnostics, not called by the kit: do the cores' TSCs agree (a line in the boot trace, from a
// detached thread), and what the TSC, clocks, a yield, atomics and float conversions cost.
void ps4_tsc_probe(void);
void ps4_timer_bench(char* out, size_t size);

// --- Ending the app ----------------------------------------------------------------------------
//
// Asks the shell to close the app (sceSystemServiceLoadExec("exit")) instead of running the
// process teardown, which crashed with the GPU and VideoOut still live. exit() and a return from
// main come here too (--wrap=exit).
__attribute__((noreturn)) void ps4_kit_exit(int status);

// --- Hooks the runtime calls (weak: each is optional) ------------------------------------------
//
// ps4_boot_trace(line)           ps4_crashlog.cpp: every log line also goes to the boot trace
// ps4_install_signal_stack()     ps4_crashlog.cpp: at the start of every new thread
// ps4_watch_thread(name)         ps4_sampler.cpp: at the start of every new thread, "(unnamed)"
// ps4_profile_thread_begin()     an app profiler: at the start of every new thread

#ifdef __cplusplus
}
#endif

#endif
