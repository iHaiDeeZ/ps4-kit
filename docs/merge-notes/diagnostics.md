# Diagnostics: crash log, sampler, burst profiler, host tools

Merged 2026-10-25 from the 12 ports' `port/ps4_crashlog.cpp` and `port/ps4_sampler.cpp`, SSBU's
`port/ps4_profile.cpp`, and the host scripts (`crashlog.sh`, `profile.sh`, `profile.py`,
`hotfuncs.py`, `decode-flight.py`, PS2's `klog-capture.py`). The ports were not changed.

| Kit file | What |
|---|---|
| `runtime/ps4_kit_diag.h` | public API, link-time configuration macros, hook prototypes; an internal section (`PS4_KIT_DIAG_INTERNAL`) with the signal-safe code-address tests the three .cpp files share |
| `runtime/ps4_crashlog.cpp` | data folder (+ one-time rename), boot trace, crash handler, signal stacks, std::terminate report, open-file tracking |
| `runtime/ps4_sampler.cpp` | 1 kHz SIGPROF sampler, thread watch + thread dumps, CPU-busy report, wait counters, hot-spot profiler |
| `runtime/ps4_profile.cpp` | burst profiler (kept: see "ps4_profile.cpp" below) |
| `scripts/diag-env.sh`, `diag.conf.example` | settings for the tools: config file, environment, command line |
| `scripts/crashlog.sh` | fetch + symbolize boot trace, crash, hang dumps, app logs, GPU fault files |
| `scripts/profile.sh`, `profile.py` | fetch + name samples.log, profile.log, profiles/profile_N.txt |
| `scripts/hotfuncs.py` | guest + x86 code of the busiest translated functions (any guest ISA llvm-mc knows) |
| `scripts/decode-flight.py` | RADV flight recorder command streams (unchanged apart from the header) |
| `scripts/klog-capture.py` | GoldHEN klog capture until the title's core dump |

Compile check (WSL): note the flag order - `$PS4_CXX_FLAGS` must come before `$PS4_BASE_FLAGS`
(libc++'s headers before the C headers). The order `$PS4_BASE_FLAGS $PS4_CXX_FLAGS` fails with
"<cstring> tried including <string.h> but didn't find libc++'s <string.h>" even for the ports'
own files (checked on Vita3K's ps4_crashlog.cpp).

```
wsl -e bash -lc 'source ~/vita3k/toolchain/env.sh; cd "/mnt/e/PS4 Projects/ps4-kit/runtime"; \
  clang++-21 $PS4_CXX_FLAGS $PS4_BASE_FLAGS -isystem $PS4_SYSROOT/include -march=btver2 -std=c++20 -O2 -Wall -Wextra -c ps4_crashlog.cpp -o /tmp/c.o'
```

All three .cpp files compile with no warnings at `-Wall -Wextra`; the header compiles as C11 and
C++20. A test link (`ld.lld -pie -T openorbis/link.x`) confirmed that missing hooks/config resolve
to 0 locally (hidden weak) and create no dynamic imports, and that a port's definitions win.

## Linker wraps

Defined (both **weak**, so the file layer's own wraps win):

- `__wrap_open(const char*, int, ...)` - calls `__real_open`, then `ps4_diag_note_open`.
- `__wrap_close(int)` - `ps4_diag_note_close`, then `__real_close`.

Referenced (weak hidden; null without the matching `--wrap`, then the plain function is used):
`__real_open`, `__real_close`, `__real_pthread_create`.

Every port's crashlog defined *strong* `__wrap_open/__wrap_close` that also did file-layer work
(netfs `/net` paths: Crema, Sonic, SSBU, M33...; `ps4_fileat_opened/closed`: HENkaku, Conker;
relative-path resolution: Conker). That work belongs to the file layer and was left out here. **The
kit's file layer (if it defines strong `__wrap_open/__wrap_close`) must call
`ps4_diag_note_open(fd, path)` after a successful open and `ps4_diag_note_close(fd)` before the
real close**, or descriptor-leak reports stay empty. With no strong wraps, the weak ones here do the
tracking (but then relative paths and /net paths get no special handling).

## What the runtime (thread start) must call

The ports' `ps4_runtime.cpp` thread trampoline (`__wrap_pthread_create`) calls, in each new thread:
`ps4_install_signal_stack()`, `ps4_watch_thread("(unnamed)")`, and (SSBU) `ps4_profile_thread_begin()`.
The kit keeps those names and signatures. The main thread gets its signal stack in the crash log's
constructor (priority 101) and its burst-profiler slot in a constructor (priority 102) - SSBU's
main() call is no longer needed. `ps4_watch_thread` calls the runtime's `ps4_heap_private_arena(name)`
if it exists (weak).

## Features and where they came from

### ps4_crashlog.cpp

| Feature | Source | Notes |
|---|---|---|
| Boot trace at priority-101 constructor, crash handler for ILL/ABRT/FPE/BUS/SEGV, regs, code addresses on the stack, `ps4_trace_callers` | all (DolphinPS4 lineage) | |
| Nested crash in the handler -> `_exit` (not return: it re-ran the faulting read forever) | all but G-Diffuser | G-Diffuser still returned to the default action |
| Fault just below rsp -> "ran out of stack" line | all but G-Diffuser | |
| Timer thread (15 s) + `ps4_dump_thread_stacks` + `ps4_crash_guest_info` after the lock-free part; `_exit` instead of the system core dump (FW 13.52 hang) | all but G-Diffuser | timer thread via `__real_pthread_create` (heap may be held) |
| Early "fatal signal" line before anything that can fault (static buffer, write only) | Crema, DawnStation | now also names the thread id |
| crash.log cannot be opened -> report into the boot trace | rpcs3 (Crema 1.69/1.73) | |
| Readability probe (pipe write, EFAULT) for the stack scan | HENkaku, Conker, SMW, SSBU, M33, Resonance, Sonic, xemu | rpcs3/Crema/PS2 lacked it |
| Signal stack per thread, freed by a pthread key destructor; guard page; deepest-use log | rpcs3 (v01.25, v01.56/57) | size configurable (default 128 KiB, rpcs3 used 512 KiB); zeroing + deepest-use log only with `ps4_diag_set_signal_stack_stats(1)` (it cost a full memset per thread) |
| Hooks `ps4_crash_guest_regs`, `ps4_crash_jit_info`, `ps4_crash_note`, `ps4_crash_restart`, `ps4_crash_guest_info` | rpcs3, Crema, PS2 (+ guest_info everywhere) | made hidden-weak (PS2's form) so a missing hook never becomes a dynamic import |
| std::terminate handler with `what()` | all | `what()` guarded by `__cpp_exceptions` (Dolphin builds with -fno-exceptions: type only); rpcs3 wrote the message to the trace twice - once now |
| Data folder worked out at first use without the heap; `ps4_data_dir()`, `ps4_crash_log_path()`, `ps4_data_dir_moved()` | HENkaku, Conker | kit names `ps4_diag_*`; the old names are kept as weak definitions |
| One-time rename of an old data folder + path rewrite in listed files | rpcs3, Crema (rewrite), HENkaku (rename + fallback), Resonance (rename) | generalized: `PS4_DIAG_OLD_DATA_DIR`, `PS4_DIAG_MIGRATE_FILES`; rewrite now streams through an 8 KiB stack buffer into `<file>.moving` + rename (was a static 1 MiB buffer and an in-place truncate) |
| Previous run's trace kept | HENkaku (`boot-trace.prev.log`), rpcs3 (`-prev`/`-game`), Crema/PS2 (`-game`/`-launcher`) | generalized: `boot-trace-game.log` when the old trace contains `PS4_DIAG_GAME_MARKER` or `PS4_DIAG_GAME_STOP_FILE` exists, else `boot-trace-prev.log` |
| Open-file tracking + `ps4_open_calls_report` | rpcs3, Crema (1.75) | `ps4_diag_note_open/close` API for the file layer; 1024 descriptors tracked (was 4096: 540 KiB of BSS for numbers the PS4 never hands out); the caller scan is probed and bounded (400 words above the frame could run off a fresh thread's stack) |

New in the kit (not in any port):

- **Text range from the linker script** (`__text_start` .. `__eh_frame_start`) instead of a guessed
  window (64 MiB / 128 MiB rpcs3 / 1 GiB SSBU, SMW). SSBU's profile.cpp already used `__eh_frame_start`
  after the 1 GiB window read unmapped memory (v00.12). The range includes .rodata (mapped, so safe
  to read; why the next point matters).
- **Return addresses only** in every stack scan (crash report, thread dumps, wait chains, open
  tracking, trace_callers): a value counts only if a `call` instruction ends right before it (E8
  rel32 and every FF /2 form, length decoded from ModRM/SIB). From SSBU's profile.cpp (v00.13),
  extended to all `call r/m` encodings. The old lists were full of stale values and .rodata
  pointers (a test on an rpcs3 crash.log listed `GCC_except_table9` as a frame). Risk: a frame
  whose call form isn't recognised would be missing; all x86-64 near calls are covered.
- **Two threads crashing at once**: the second waits (up to 20 s) for the first one's report
  instead of `_exit`ing through the shared depth counter, which cut the first report off.
  A fault inside the reporting thread itself still exits at once.
- **Run-time switches**: crash log (default on; off = no files, the app still ends itself), boot
  trace (on), open tracking (on). Thread dumps (on) live in the sampler file.
- Boot-trace open is thread-safe (atomic state, recursion from the same thread returns -1) - the
  ports raced two first loggers into opening it twice.
- `ps4_boot_tracef` (printf form).

### ps4_sampler.cpp

| Feature | Source | Notes |
|---|---|---|
| 1 kHz SIGPROF sampler, 10 s windows, every bucket seen twice (up to 1500), module list | all | 16 threads (was 4-12); thread names copied (rcps3 kept the caller's pointer) |
| Pause/resume switch `ps4_sampler_set_enabled` | rcps3, Crema, PS2... (12 ports) | |
| Wait chains for samples in system libraries ("wait" lines), stack bounds from `pthread_attr_get_np` | rpcs3 (v01.37, v01.51) | chain is now call-checked return addresses; M33's "word at rsp" and frame-pointer walk are covered by that (frame pointers are omitted at -O2 and M33's walk was unbounded above the stack: up to rsp + 1 MiB) |
| Re-registering by name takes over the slot | M33, Resonance, SMW, SSBU, Conker, Sonic, xemu, HENkaku | |
| libkernel entry points (`symbol` lines) for naming sys samples | Crema | profile.py now uses them (Crema's profile.py didn't) |
| JIT owner hook (`ps4_jit_owner`) + `hotfuncs.bin` (`ps4_jit_dump_function`) | Crema, PS2 | output `jit 0x.. guest XXXXXXXX +off` (was `ppc`); scripts accept both |
| jitcode.bin / mem1.bin every 60 s | all (Dolphin) | only written when there is JIT code or guest RAM (SMW/SSBU skipped the dump without guest RAM; others wrote an empty file each minute); switch `ps4_sampler_set_code_dumps`; `ps4_sampler_set_guest_ram` + the old globals |
| Thread watch + thread dumps, `_umtx_op` decoding, sane-stack checks | all; slot reuse + generations rcps3 | 128 slots (rcps3 96 + reuse); "in a system call" marker on the rip line; the dumping thread skips itself (the crash handler signalled its own thread) |
| Hooks `ps4_recompiler_status`, `ps4_hang_guest_threads`, `ps4_crash_jit_info` in dumps | rpcs3, Crema, PS2 | hidden-weak |
| CPU-busy report, `ps4_thread_cpu_tick`, SIGUSR2 query for threads that never tick | rpcs3 | tick re-finds its slot when the slot changed hands (generation check) |
| `ps4_wait_add/total` | all | 16 app-defined slots (was 6 with Dolphin meanings) |
| Hot-spot profiler (`ps4_hot_profiler_set_enabled`) | Conker (v00.13) | restarted-syscall detection from SSBU; slot generations; shares the dump signal through a bounded lock with thread dumps (they raced on the same per-thread fields) |
| `ps4k_` asm-label declarations of sce functions | Sonic | the toolchain headers' prototypes clashed in SRB2's build |

Overhead: nothing is allocated until a profiler is switched on. The sampler's per-thread buffers
(384 KiB each), the report scratch (~700 KiB) and the hot-spot profiler's 2 MiB are calloc'd on first
use; in the ports they were static BSS (about 7 MiB in rpcs3, with its 1 MiB migration buffer and the open-file table, whether profiling or not). All signal
handlers run on the thread's signal stack (SA_ONSTACK) and keep SA_RESTART.

### ps4_profile.cpp (kept)

Kept because it does what the sampler can't: every thread of the app (TLS slot index, no search
in the handler), a few seconds at 250 Hz on demand (SSBU starts one when the frame rate drops),
system samples named by module with the code bytes around the busiest, and the cleanest caller
detection (where the call-check came from). Changes:

- Signal **SIGVTALRM (26)** instead of SIGPROF: SSBU's sampler and burst profiler both installed a
  SIGPROF handler, the later one silently replacing the other.
- Thread filter by name prefix via `ps4_profile_set_threads` (SSBU's list was compiled in:
  `guest, gpu, vsync, shader compiler, status, window, dxvk-cs, dxvk-shader, dxvk-descriptor` -
  SSBU must pass it when migrating). No filter = every registered thread (risk below).
- Thread names from the hook `ps4_profile_thread_description(tid)` (was SSBU's `rt_thread_description`),
  else `scePthreadGetname`.
- Off by default (`ps4_profile_set_enabled`); `ps4_profile_start` returns false while off.
- Sample rings allocated per sampled slot on first use (SSBU: 10 MiB static); no `new` per run;
  data folder from `ps4_diag_data_dir()`; finish line to the boot trace instead of stderr.

## Left out (app-specific) and why

- File-layer work inside `__wrap_open/__wrap_close` (netfs, fileat, relative paths) - see "Linker wraps".
- Hard-coded data folders (`/data/rpcs3`, `/data/Crema`, `/data/dawn`, `/data/HENkaku`, `/data/SSBU`,
  `/data/srb2`, `/data/xemu`, `/data/gdiffuser`...) -> `PS4_DIAG_DATA_DIR`. Default `/data/ps4-kit`
  (or `-DPS4_KIT_DATA_DIR=` when building the kit).
- Per-app migration file lists (rpcs3: games.yml, config.yml, launch.txt, vfs.yml; Crema:
  launcher.ini, launch.txt, played.ini, settings.xml) -> `PS4_DIAG_MIGRATE_FILES`.
- rpcs3's `"] game: "` marker and `last-stop.txt` -> `PS4_DIAG_GAME_MARKER` / `PS4_DIAG_GAME_STOP_FILE`.
- Emulator thread-name comments, Dolphin's wait-slot meanings, Crema/rpcs3/SMW guest decoding: only as
  hooks. crashlog.sh's `guest-code.py` call (rpcs3) -> `PS4_CRASH_HOOK`; each port's guest section
  heading (`== PSP CPU at the crash`, `== SH4 at the crash`, `== guest CPU at the crash`, Crema's
  log.txt `Active PPC instance`) -> `PS4_GUEST_SECTION`; RPCS3.log's error grep -> `PS4_APP_LOG_ERRORS`.
- SSBU profile.py's fixed `~/ssbu-build` paths and NRO lookup -> `--elf`, `--ro-log`, `--ro-dir`, or
  a generic `--regions` file.
- G-Diffuser's older copies: nothing in them the others lacked.

## Port settings when migrating (from their copies)

| Port | Settings |
|---|---|
| Crema | `PS4_DIAG_DATA_DIR("/data/Crema")`, `PS4_DIAG_OLD_DATA_DIR("/data/CemuPS4")`, `PS4_DIAG_MIGRATE_FILES("launcher.ini","launch.txt","played.ini","settings.xml")`, `PS4_DIAG_GAME_MARKER("] game: ")`, `PS4_DIAG_GAME_STOP_FILE("last-stop.txt")` |
| rpcs3 | `/data/rpcs3`, old `/data/rcps3`, files `games.yml, config.yml, launch.txt, vfs.yml`, same marker/stop file, `ps4_diag_set_signal_stack_size(512*1024)` before threads start (its access violation handler) |
| HENkaku | `/data/HENkaku`, old `/data/Vita3K` (no file rewrite) |
| Resonance | `/data/Resonance`, old `/data/DeCast-S4` |
| SSBU | `/data/SSBU`; `ps4_profile_set_threads` with its 9 prefixes; `ps4_profile_set_enabled(1)` where it starts profiles |
| others | their `/data/<name>` (Conker, March33, SMW, srb2, xemu, dawn, gdiffuser) |

## Output changes the ports' tools will notice

- Previous trace: `boot-trace-prev.log` (HENkaku/Conker wrote `boot-trace.prev.log`, Crema/PS2
  `boot-trace-launcher.log`).
- Stack lists hold return addresses only; crash header names the thread id.
- `jit ... guest XXXXXXXX +off` (was `ppc`); hot-spot profile header no longer says "Conker".
- `open() files still open: N in M groups` (dropped "(not file streams)").
- Burst profiles have a `threads sampled:` header line.

## Pitfalls to remember (kept as comments in the code)

- Threads "stuck" in `mspace_malloc` in a profile were spinning on dlmalloc's spin lock
  (USE_SPIN_LOCKS livelock); the fix was blocking locks in the heap, not in the profiler. Signals
  make it worse (every interruption lengthens a held lock).
- `libkernel 0x800003710` at the top of a profile = a restarted system call (`_umtx_op` stub at
  +0x371a, the thread put back on its `syscall` by SA_RESTART): a wait, not work. The kit marks
  both "after syscall" and "on syscall" as waiting.
- The 1 kHz sampler froze SMW after ~40 s (a GPU-waiting thread in an interrupted libkernel wait):
  register only CPU-bound threads; prefer the hot-spot or burst profiler with a thread filter.
- `pthread_getcpuclockid` fails on the PS4: busy threads tick their own clock, others are asked by
  SIGUSR2.

## Signals used

SIGILL/ABRT/FPE/BUS/SEGV crash handler; SIGPROF (27) sampler; SIGVTALRM (26) burst profiler;
SIGUSR1 (30) thread dumps and hot-spot profiler; SIGUSR2 (31) CPU-busy query. All SA_SIGINFO |
SA_RESTART | SA_ONSTACK (crash: SA_SIGINFO | SA_ONSTACK).

## Host tools

Settings come from (lowest to highest) a `kit.conf` (`-c`, `$KIT_CONF`, `./kit.conf`, or
`scripts/../kit.conf`), the environment, then options. Keys: see `scripts/diag.conf.example`
(PS4_HOST, PS4_FTP_PORT, PS4_DATA_DIR, PS4_TITLE_ID, PS4_APP_NAME, PS4_BUILD_ROOT, PS4_ELF_DIR,
PS4_LOGS_DIR, PS4_ELF, PS4_APP_LOGS, PS4_APP_LOG_ERRORS, PS4_GUEST_SECTION, PS4_CRASH_HOOK,
PS4_GAME_STOP_PATTERN, PS4_SCREENSHOTS, PS4_SCREENSHOTS_DOWNLOAD, PS4_GPU_DIR, PS4_JIT_MAP,
PS4_SYMBOLIZER). No console address is built in: a fetch without one stops with a message.

```
crashlog.sh -H 192.168.0.212 -d /data/HENkaku            # fetch + symbolize (ELF from the trace's version)
crashlog.sh -c ~/cemups4/kit.conf -n                     # re-read what was fetched last time
profile.sh -c kit.conf -b -- --top 40 --window 60-120    # sampler + hot-spot + burst profiles
python3 profile.py --elf v01.23.elf samples.log profile_3.txt
python3 hotfuncs.py hotfuncs.bin samples.log --triple powerpc --thread core=2
python -I klog-capture.py --host 192.168.0.212 --title DAWN00001 klog.txt   (or the old positional form)
python -I decode-flight.py logs/gpu/ib-2.bin 1234
```

Tested offline on the ports' own logs: crashlog.sh `-n` on an rpcs3 crash.log + Crema hang.log;
profile.py on rpcs3 and Crema samples.log and an SSBU burst profile (with `--ro-log`); hotfuncs.py
on a Crema hotfuncs.bin. Not tested against a console (no fetch was run).

Script fixes picked up on the way: symbolizing adds `__text_start`'s ELF address (SSBU's profile.py
did; 0 with OpenOrbis' link.x, so harmless today); Conker's crashlog.sh was a copy of HENkaku's and
fetched `/data/HENkaku` - with settings that can't recur.

## Open questions

1. **`__wrap_open/__wrap_close` ownership.** Does the kit's file layer define strong wraps? If so it
   must call `ps4_diag_note_open/close`. If two helpers both define weak ones, the linker keeps
   whichever object comes first.
2. **`ps4_data_dir()`**: defined weak here (HENkaku/Conker callers). If the runtime helper defines a
   strong one with a different folder, the crash files still use `ps4_diag_data_dir()`.
3. **Signal stack default 128 KiB**: between the ports' 64 KiB and rpcs3's 512 KiB. Apps whose
   SA_ONSTACK handlers do real work must raise it; check with `ps4_diag_set_signal_stack_stats(1)`.
4. **Burst profiler default "all threads"**: safe only for short profiles; SMW-style GPU waits can
   freeze. Should the kit default to registered/watched threads only, or require a filter?
5. **Call-check on hardware**: reads up to 7 bytes below candidate values inside the eboot image
   (text + rodata, mapped). SSBU did the same on hardware; the wider FF /2 decoding is untested there.
6. SIGVTALRM for the burst profiler is untested on the PS4 (nothing in the ports used it).
7. Crash log "off": still installs the handler and ends the app itself (FW 13.52 core dump hang). If
   "off" should mean "the system's crash report", that needs SIG_DFL instead - not recommended.
