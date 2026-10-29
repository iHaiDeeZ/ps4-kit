# Runtime core merge: ps4_runtime.cpp, ps4_dlmalloc.c, ps4_crt1.S, ps4_string.c, ps4_random.c

Merged 2026-10-25 from the `port/` copies of all 12 ports. Public API: `runtime/ps4_kit.h`.

## What the copies were

`ps4_runtime.cpp` (compared with CR stripped and the per-app log tag / mapping names normalised):

| Port | Lines | Relation |
|---|---|---|
| Sonic (10-10) | 780 | base; only difference from Resonance: `ps4k_` asm-alias declarations (PacBrew headers) |
| Resonance (10-12) | 780 | base |
| March33 | 787 | base + `ps4_heap_footprint` |
| SMW | 796 | + 256 MiB system-pool cap, `ps4_new_thread_cores`, 4 MiB stacks, `ps4_boost_thread` |
| SSBU | 798 | SMW + `ps4_profile_thread_begin()` call (strong, not weak) |
| Vita3K | 797 | + 256 MiB cap, `ps4_heap_footprint`, `ps4_boost_thread` |
| Conker | 796 | Vita3K + 0x7F cores, `ps4_thread_entry` (no boost) |
| xemu-ps4 | 801 | + xemu arena names, PROT_NONE = reservation, mmap log throttling, `ps4_boost_thread` |
| G-Diffuser-PS4 | 879 | + fixed heap address, exec-memory probe (5 methods, sigsetjmp), JIT alias probe; pthread_create wrapper removed |
| PS2 (10-21) | 1122 | an older snapshot of CEMUPS4: nothing that CEMUPS4 lacks |
| CEMUPS4 (10-23) | 1161 | + bounded logStack, overflow wait, 10 MiB x 20 arenas, per-thread op counters, exec-memory probe (3 sources, rip/rsp fix-up), 0x7F cores, TSC probe, boost, heap report, timer bench |
| rcps3 | 1562 | CEMUPS4's exec probe + direct-memory main heap, 6 releasable overflow heaps, in-use accounting, sampling heap profiler, big compiler arenas, program-image guard (5 more wraps), RPCS3 arena names |

So the big outliers' additions are mostly generic. What was app-specific in them is the thread-name lists and the
RPCS3 heap order. Those are now settings (see below).

`ps4_dlmalloc.c`: 4 versions. The base, then `USE_SPIN_LOCKS 0` (Conker, CEMUPS4, rcps3, PS2), then `ps4_heap_top_free`
(CEMUPS4 only).
`ps4_crt1.S`, `ps4_string.c`: identical in all 12, copied byte for byte (CRLF kept).
`ps4_random.c`: identical in 11. Sonic declared `sysctl` by hand because PacBrew's OpenOrbis has no `<sys/sysctl.h>`, while the
others include it. The kit version uses `__has_include` and works with both.

## Features and where they came from

| Feature | From | Kit form |
|---|---|---|
| dlmalloc mspace heap, guard page, away from 0x200000000, self-test | all | as is |
| `USE_SPIN_LOCKS 0` (blocking dlmalloc locks) | Conker, CEMUPS4, rcps3, PS2 | **in**, comment merged (Crema 1.61, Conker v00.09/10, TOTK) |
| `ps4_heap_top_free` | CEMUPS4 | in (used by `ps4rt_heap_report`) |
| static assert: `pthread_mutexattr_t` >= 8 bytes | new | dlmalloc's `pthread_init_lock` (used when spin locks are off) keeps the attr on the stack. Built against musl's 4-byte type it would overwrite 4 bytes of stack on every new mspace, so the build fails instead |
| main heap ≤ 256 MiB in the system pool | SMW, Vita3K, Conker, SSBU | default `system_pool_max` (512 in Sonic/Resonance/March33/xemu/G-Diffuser/CEMUPS4/PS2) |
| main heap in direct memory first | rcps3 | `source_order = PS4_KIT_HEAP_DIRECT_FIRST` |
| fixed heap base address | G-Diffuser | `fixed_address` (main heap only, flexible pools only; G-Diffuser put it in `reserveAwayFromKernel`, so every mmap tried it first) |
| last-resort direct heap placed away from the kernel | rcps3 (`createDirectHeapAway`) | used for every direct heap (Sonic's `createDirectHeap` left placement to the kernel) |
| several overflow heaps, released when empty, direct reserve | rcps3 | in. `max_overflow_heaps` (6) and `overflow_direct_reserve` (192 MiB) are settings |
| threads wait for an overflow heap being created | CEMUPS4 | covered by rcps3's lock and "another thread added one" retry; the lock now sleeps instead of `sched_yield` |
| per-thread arenas, reuse of exited threads' arenas | all | in. Name lists are app settings; 10 MiB x 20 (CEMUPS4/rcps3) are the defaults |
| big compiler arenas (32 MiB x 10) | rcps3 | `ps4_kit_set_big_arena_threads` |
| per-thread op counters in `checkHeap` | CEMUPS4 | in |
| in-use accounting (`ps4_heap_in_use_mib`) | rcps3 | in, always on (thread-local deltas) |
| sampling heap profiler | rcps3 | in, **off by default** (`ps4_kit_set_heap_profiler`); tables (~3.5 MiB) mapped on first use instead of sitting in `.bss` |
| bounded stack scans | CEMUPS4 (QueryMemoryProtection), rcps3 (pthread stack) | merged: pthread stack end, else the mapping's end, else the page |
| code range for stack scans | new | `__text_start`..`__eh_frame_start` (weak), instead of a fixed 16 MiB (logStack) / 128 MiB (profiler) |
| executable-memory probe | CEMUPS4/rcps3 (system pool, regular, direct) + G-Diffuser (mmap+mprotect, named flexible, JIT shm, alias log) | one probe, 6 sources in that order. FreeBSD `sigaction` declared with its own struct, so it does not depend on which `signal.h` the build uses. G-Diffuser's version used musl's `SIGBUS` (7, which is `SIGEMT` on FreeBSD) |
| `ps4_exec_probe()` | G-Diffuser | in, returns the source index |
| program-image guard (munmap/mprotect) | rcps3 | in, default on; **narrowed**: mprotect is refused only on read-only segments (code, rodata), munmap on any segment |
| PROT_NONE = reservation only | xemu | `ps4_kit_set_mmap_reserve_prot_none`, default **off** (an app that mprotects such a range needs it backed) |
| mmap log throttling (first 32, every 256th) | xemu | in, for all mmap log lines |
| `ps4_heap_footprint` (main + overflow) | March33, SMW, SSBU, Vita3K, Conker | in |
| `ps4rt_heap_footprint`, `ps4rt_heap_report` | CEMUPS4 | in, the report now covers every overflow heap |
| `ps4_new_thread_cores` | SMW, SSBU | in; default = the process's cores read at the first allocation (0x7F with the 7th core, else 0x3F). Conker/CEMUPS4/rcps3 hard-coded 0x7F, the rest 0x3F |
| minimum thread stack | all (2 MiB), SMW/SSBU 4 MiB | `ps4_kit_set_min_thread_stack`, default 2 MiB |
| `ps4_thread_entry` (backtrace stop, no tail call) | Conker | in, for every port |
| `ps4_profile_thread_begin` at thread start | SSBU | weak hook |
| `ps4_boost_thread` (priority 650) | SMW, SSBU, Vita3K, xemu, CEMUPS4, rcps3, PS2 | in, `ps4_kit_set_boost_priority` |
| TSC clock_gettime | all | as is. `ps4_clock_reads` is only counted when `ps4_kit_set_count_clock_reads(1)` |
| `ps4_tsc_probe`, `ps4_timer_bench` (with float tests) | CEMUPS4 | in (diagnostics, never called by the kit) |
| `pthread_once` (4-byte once) | all | as is, waiters back off to sleep |
| `__cxa_thread_atexit_impl` | all | as is |
| libc malloc replacement table hooks (`ps4_replace_*`) | all | as is; stats count the overflow heaps; usable-size/reallocalign check ownership first |
| `__wrap_malloc_usable_size` | new | optional; musl's would read our chunks as its own |
| `ps4k_` asm-alias kernel declarations | Sonic | used for every kernel call, so no header prototype can clash |
| log tag / mapping names | each port had its own (`[srb2]`, `[cemu]`, `"smw heap"`...) | `log_tag` setting (default `ps4`): `[tag] heap: ...`, `"tag heap"`, `"tag mmap"`, `"tag code"` |

### Fixes I added (same hardware fact as the dlmalloc freeze)

Every `sched_yield()` wait loop in the originals had the same priority-inversion risk that froze dlmalloc's spin locks.
These are the arena free-list lock, the arena key creation, the overflow-heap lock, the wait for unpinning, the profiler's site
lock, and the `pthread_once` waiters. They now use `Backoff`: 64 pause spins, then 50 µs `nanosleep`
(the OpenOrbis `nanosleep` jumps to libkernel's `_nanosleep`). `sched_yield` is left only in the TSC probe's one-off core switch and in the benchmark.

Smaller fixes: a reservation the kernel placed too low is unmapped (it used to leak). A failed fixed-address heap mapping
unmaps its range so the next size can try it. A failed arena `create_mspace_with_base` frees its block. Nested allocations made
during heap creation no longer try to map overflow heaps or log a dlmalloc usage error for a null mspace. All globals that malloc touches
are `constinit`, and the object file has no static initializer.

## Lazy memory zero race

None of these files has the pattern. Fresh mappings are not cleared in place. The kernel zero-fills them, and no
other thread can see them before they are published. dlmalloc runs with `HAVE_MMAP 0`, so `calloc` always clears its own chunk.
The pattern lived in the ports' guest-memory managers (SSBU v00.17, TOTK, BOTW), which are outside the runtime core.

## Left out

- `ps4_fault_count` (Sonic and all others: "fastmem faults seen by Dolphin's handler"). It is Dolphin-specific, and nothing outside
  `ps4_runtime.cpp` in any port references it. A port that needs it can define its own counter.
- The per-app arena name lists are not compiled in. No thread gets an arena unless the app lists it. To get the old behaviour,
  a port passes its list, for example:
  - DolphinPS4 (Sonic, Resonance, March33, SMW, SSBU, Vita3K, Conker, G-Diffuser kept these unchanged): `"CPU thread", "CPU-GPU thread", "Video thread", "VK submission thread", "AsyncShaderCompiler Worker", "DVD thread", "Audio thread - PS4"`
  - Crema (CEMUPS4): `"PPCRecompiler", "OSSched[core=0]", "OSSched[core=1]", "OSSched[core=2]", "LatteThread", "LatteDriver", "compilePl", "vkShaderComp", "plCacheCompiler", "vkDriverPlCache", "fileCache", "PS4AudioOut", "H264Decoder"` (PS2 is the same minus `LatteDriver`)
  - xemu: `"CPU 0/TCG", "ALL CPUs/TCG", "nv2a.pfifo_thread", "mcpx.apu_thread", "qemu_main"`
  - RPCS3: the Crema list plus `"PPU[*", "SPU[*", "rsx::*", "RSX*", "cellAudio*", "VK *", "PPU Worker*", "SPU Worker*", "Shader*", "PS4AudioOut*"`; big: `"PPUW.*", "SPRX Worker*", "SPUW.*", "SPU Worker*", "PPU Worker*"`
- G-Diffuser's removal of the pthread_create wrapper was not taken. Its build also stopped passing `--wrap=pthread_create`, and the
  one-commit history gives no reason.

## Settings per port, to keep each port's current behaviour when it migrates

| Port | Setting |
|---|---|
| Sonic, Resonance, March33, xemu, G-Diffuser, CEMUPS4, PS2 | `system_pool_max = 512 MiB` |
| rcps3 | `source_order = PS4_KIT_HEAP_DIRECT_FIRST` (direct_max 1 GiB = default) |
| G-Diffuser | `fixed_address = 0x1040000000` |
| SMW, SSBU | `ps4_kit_set_min_thread_stack(4 MiB)`; they already set `ps4_new_thread_cores` |
| Sonic, Resonance, March33, xemu, G-Diffuser | before: 0x3F for new threads (kit default reads the process's cores) |
| xemu | `ps4_kit_set_mmap_reserve_prot_none(1)` |
| ports whose code reads `ps4_clock_reads` | `ps4_kit_set_count_clock_reads(1)` |
| rcps3 | `ps4_kit_set_heap_profiler(1)` if the profile lines are wanted |

## `__wrap_` functions defined (the build must pass `--wrap=<fn>`)

Required. They reference `__real_<fn>`, or are the reason the runtime exists:

`malloc free calloc realloc memalign __memalign mmap munmap mprotect sceKernelMunmap sceKernelMprotect
sceKernelMtypeprotect clock_gettime pthread_once pthread_create`

Optional (defines no `__real_`): `malloc_usable_size`.

Without `--wrap` for any of `mmap munmap mprotect sceKernelMunmap sceKernelMprotect sceKernelMtypeprotect clock_gettime
pthread_create`, the link fails with an undefined `__real_<fn>`. The five image-guard wraps are new for every port except rcps3.
Not wrapped here: `open`/`close` (ps4_crashlog.cpp), the network and file wraps (io layer).

Also defined for other parts of the program: `__cxa_thread_atexit_impl`, `ps4_heap_error` (called by dlmalloc), and the
`ps4_replace_*` table entries (referenced by `ps4_crt1.S`).

## Build notes

- The compile command in the task puts `$PS4_BASE_FLAGS` before `$PS4_CXX_FLAGS`. Then the C headers come before libc++'s, and
  `<atomic>` fails ("`<cstring>` tried including `<string.h>`"). The original `Vita3K/port/ps4_runtime.cpp` fails the same way
  (24 errors). With `$PS4_CXX_FLAGS` first, `ps4_runtime.cpp` compiles with `-Wall -Wextra` and no warnings. The C files and `ps4_crt1.S`
  compile as given. `ps4_random.c` also compiles without the libc-overlay. `ps4_dlmalloc.c` deliberately does not (static assert).
- The `.cpp` needs the libc-overlay's FreeBSD-sized pthread types (same as the ports).

## Open questions

1. **Weak undefined hooks.** `ps4_boot_trace`, `ps4_install_signal_stack`, `ps4_watch_thread`, `ps4_profile_thread_begin`,
   `ps4_kit_configure_heap` and `__eh_frame_start` are weak references. Every port linked crashlog and the sampler, so nobody
   has tested a link where a hook is missing. Check that create-fself accepts an unresolved weak symbol, or that lld resolves it to 0
   without a dynamic import.
2. **The default system-pool cap of 256 MiB.** Ports that used 512 (Crema, DawnStation, Dolphin) will spill to overflow heaps
   sooner. That works, but costs a failed main-heap attempt per spilled allocation. Is 256 right as the kit default?
3. **Image guard default on.** It is safe for every port known to me, but a port that patches or re-protects its own code
   segment (a self-modifying recompiler) would be refused, with a "GUARD:" log line. Turn it off with `ps4_kit_set_image_guard(0)`.
4. **`ps4_new_thread_cores` default.** It comes from the main thread's affinity at the first allocation. Conker's notes say the
   main thread starts with 0x7F. If a firmware starts it with fewer than 6 cores, the kit falls back to 0x3F. This is unverified on
   a console without the 7th core.
5. **Exec probe order.** The kit tries the system pool first (CEMUPS4 order), while G-Diffuser tried a plain mmap first. On
   consoles where the system pool runs code, G-Diffuser's JIT will now get system flexible memory instead of regular. This
   matches Crema, but has not been tested with G-Diffuser.
6. Direct memory used for exec blocks is never released on munmap (as in CEMUPS4/rcps3). Is it zero-filled? No caller needs
   that, but nobody has checked.
