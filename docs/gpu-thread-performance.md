# The render thread's CPU cost on the PS4 (Jaguar + RADV)

Findings from Crema (Cemu, CEMUPS4) for any port whose render thread records many Vulkan draws.
"Measured" means on the console (base PS4, .90); "not measured yet" marks changes that are built but
waiting for a console run.

## Where a draw-heavy render thread spends its time (measured)

Breath of the Wild, open world, 2026-11-12 (Crema Lab 03.32): ~4,600 draws a frame, ~93k a second,
6.2 µs per draw on the render thread (LatteThread), which was busy 99-100% at 20.7 FPS. The kit's
sampler (`runtime/ps4_sampler.cpp`) for that thread, gameplay only:

| Part | Share |
|---|---|
| RADV/Mesa (draw, pipeline bind, state emission, descriptor sets) | 27% |
| the emulator's buffer cache and uniform uploads | 14% |
| the emulator's Vulkan backend | 13% |
| parsing the game's GPU command buffers | 13% |
| textures (lookups, change hashing, copies) | 13% |
| system library (3.7% `sched_yield` in the idle loop) | 6% |
| memcpy/memset/bcmp | 5% |

- `vkCmdDrawIndexed` alone is ~2 µs a draw on a 1.6 GHz Jaguar. Inside RADV the time is flat:
  `radv_emit_all_graphics_states` 7.3% (spread over its 35 KB, no hot spot), `radv_CmdDrawIndexed`
  3.3%, `radv_CmdBindPipeline` 3.0%, `radv_bind_descriptor_sets` 2.2%, shader binds ~3%.
- Mesa is already built `-O3 -march=btver2 -DNDEBUG` (meson `buildtype=release`), so compiler flags
  have nothing left to give. LTO isn't possible with PacBrew's ld.lld 12 (it can't read clang 21
  bitcode).
- `use_global_bo_list` is on (`ac_drm_query_has_vm_always_valid` sets it in `ac_ps4_drm.c`), so
  descriptor set binds don't hash BOs into a per-command-buffer list.
- RADV keeps a pipeline's non-dynamic state in the command buffer's dynamic state:
  `radv_bind_graphics_pipeline` copies and compares all of it at every pipeline change, and the next
  draw re-emits what differs. An app that uses few dynamic states pays this at every pipeline switch.
- Each `vkCmdBindDescriptorSets` with dynamic offsets builds a 16-byte buffer descriptor per offset
  (`ac_build_raw_buffer_descriptor`), then the next draw uploads them (`radv_flush_dynamic_descriptors`)
  and re-emits the user-data pointers. That's cheap per offset, but it happens on nearly every draw
  when uniforms change per draw.

With a profile this flat, no single fix removes a third of the thread's time.

## Recording Vulkan commands on a second thread (measured)

Crema's `VulkanDriverThread.cpp`: the render thread copies each `vkCmd*` call's arguments into a 4 MiB
single-producer ring, and a "driver" thread makes the real calls. It syncs before submit, present,
waits, command pool calls, every destroy/free, and rare commands with `pNext` chains.

- It works as designed: the render thread's time per draw went from 6.9 to 3.5 µs, and the thread was
  idle 31% instead of never.
- It made BotW slower (17-19 FPS instead of 19-20) while the emulated CPU threads were also at
  their limit. A Jaguar core given to the driver thread is one the other busy threads lose. Offloading
  pays only when the render thread is the sole limit.
- So it's now chosen at run time ("Auto"). Once a second, at a frame boundary on the render thread:
  - after 4 busy seconds (render thread idle under 5%), it's tried on: 1 s to settle, then 4 s measured;
  - it's kept only at 3% more frames or better, else turned off, with the next try after 1, 2, 4,
    then 8 minutes;
  - while it's on, every 90 s a trial with it off checks it still helps.
  Turning off = wait for the ring to drain, then make calls directly again (the hooks pass through
  when the thread-local flag is off). A port at 60 FPS has render-thread idle time, so it never
  triggers. Measured (BotW open world, Crema Lab 109ee892, 2026-11-14): with the driver thread on, the render
  thread went to 2.9 µs a draw and 40% idle, but the game stayed at 21.5-22.5 FPS; off, it was 22.7
  FPS at 6.2 µs and 1% idle. Auto's 90-second check turned it off, and a later trial lost 6% and was
  dropped. Later the render thread had 11-14% idle time with it off: the emulated CPU (main thread
  running 87%, its core 2 at 100%) was the limit. Freeing the render thread shows in FPS only where
  it alone is the limit; Auto's on/off trials are what find out.
- Crema Lab v03.35 (2026-11-15, BotW open world, 30 minutes): Auto turned it on after the first
  busy stretch and every 90-second check kept it: 25.7-26.8 FPS with it, 23.0-24.4 without, the
  render thread at 3.1 µs a draw and 33-35% idle. Same game, same setting, a different answer than
  on 11-14: the CPU-side fixes in between moved the limit. A fixed on/off choice would have been
  wrong one of the two days.

## Data another core just wrote (not measured yet)

The single hottest instruction of the command parser was the load of each packet header: the
game's main thread writes the command buffer on another core just before, so the render thread
waits for the cache line, worse across the two 4-core modules. The parser reads in order, so
`_mm_prefetch(next + 384, _MM_HINT_T0)` after each packet asks for the lines before they're needed.
A prefetch can't fault, even past the buffer's end.

## Lookup caches in front of trees (not measured yet)

A direct-mapped "last answer per page" table in front of an interval tree answered 99% of lookups on
a still screen but only 73% in the open world. Two causes:
1. 1024 entries for 1 KiB pages alias every 1 MiB;
2. a global generation counter, bumped by every removal, made all entries stale at once, while ranges
   come and go constantly in a streaming game.

Now: 4096 entries, and each removal clears only the entries of the removed range's own pages (a hint
for a range was set for an address inside it). Every removal must go through that one helper, or a
stale pointer is used after its object is deleted.

## Games that change state on nearly every draw (measured, Donkey Kong Country: Tropical Freeze)

DKC: Tropical Freeze, Crema Lab 109ee892, 2026-11-14: 60 FPS, about 20,000 draws a second, but
104,252 draws in 85,675 "runs" (a run = draws without state changes between them; BotW has 3.5 draws
per run). Nearly every draw takes the full setup path, so a draw costs the render thread 13 µs,
twice BotW's 6.2. The render thread still had 43-54% idle time; the busiest emulated core ran ~80%.
Optimizing the per-run path (not only the draw-to-draw fast path) is what games like this need.

Its profile (render thread busy time): the largest single cost was processing the game's data-cache
flush queue, 4.7% of all samples (about 11% of the busy time). Every flushed 1 KiB page walked
the interval tree on its own, and games flush whole buffers at once. Now the pages are sorted and
merged into runs, with one tree query per run. That's exactly equivalent, because invalidation only
widens a page-aligned range (Crema Lab 009b9fa9, not measured yet).

## A direct-access buffer path and why it waits

Upstream Cemu has a mode where the Wii U memory is imported as one Vulkan buffer
(VK_EXT_external_memory_host) and draws read vertex and uniform data from it directly, with no
buffer cache. On the PS4 the guest memory is already direct memory (WB onion) and GPU addresses
are CPU addresses. The kit's `ac_drm_create_bo_from_user_mem` returns ENOSYS although RADV
advertises the extension (has_userptr). It could be made to work: map the pages GPU-readable and
use the pointer as the GPU address. The blocker is correctness, not the PS4: GPU retirement
timestamps (EVENT_WRITE_EOP) are written when the render thread parses them, not when the PS4 GPU
has finished. A game would overwrite buffers the GPU hasn't read yet. Timestamps would have to wait
for the PS4 GPU's fences first. Upstream left the mode commented out.

## The emulated CPU side (measured BotW, Crema Lab 009b9fa9)

Open world at 23.8-24.0 FPS; the game's main thread ran 87%, emulated core 2 was pinned at 100%.
On core 2's host thread: translated game code 70%, HLE dispatch ~6%, and 4.9% spinning in the
scheduler lock's `pause` loop, waiting for another core to release it. The lock is a single
global spin lock taken by every guest thread switch, event, message, mutex, and by every
OSYieldThread's look at its core's run queue. BotW's spin loops yield tens of thousands of times a
second. Now the yield looks at the run queue without the lock first (guest memory is never freed,
so a racy read is safe; a stale answer costs one yield), with the locked look kept as the fallback
for an implausibly long walk (Crema Lab dda7e4e5, not measured yet). General lesson: on Jaguar a
global lock that many cores take at high rates costs more in waiting than in the lock itself;
take reads that only decide a hint out from under it.

Profiler caveat: with the profiler on, every HLE call goes through a timed path (10-17% of a core
in `PPCInterpreter_callHLETimed`), so HLE costs look larger than in normal play.

## Calling conventions at the JIT's helper calls (built, not measured yet)

Cemu declares its recompiler helpers `ATTR_MS_ABI` (the Windows x64 convention), so the generated
code calls them the same way on every OS. Compiled for the PS4 (SysV), such a helper that calls
ordinary code must save and restore XMM6-XMM15, RSI and RDI itself: 20 vector memory operations
per call. The HLE entry (`PPCRecompiler_virtualHLE`, every guest OS call: several hundred thousand
a second on a busy core) showed it: 6.2% of BotW's main core inside that function, mostly the
prologue and epilogue. In the generated code an HLE call is always followed by a jump through the
jump table to a segment that loads its registers from the CPU state, so nothing needs the XMM
registers kept. Crema now calls a SysV entry there (arguments in RDI/RSI); R14, which holds the
stack pointer across the call, is callee-saved in both conventions (Crema Lab v03.34, md5
f7bd85c7). Measured, BotW open world with the profiler on: the HLE entry went from 6.2% to
3.2% of the main core (2.2% from 3.6% on core 2), and core 2's scheduler-lock spin from 5.8% to
2.0% after the lockless yield look. Helpers called in the middle of generated code (mftb's `PPCRecompiler_GetTBL/GetTBU`)
still pay this cost: there the register allocator may rely on XMM6-15 surviving, so changing them
needs the allocator's call-clobber list changed too. Any port with a JIT that calls MS-ABI helpers
from SysV code pays the same toll.

## CALL/RET pairing for a JIT's out-of-line returns (built, not measured yet)

Crema's recompiler (Guest Returns: Predicted) turns guest calls into host CALLs and guest
returns into RETs, so the CPU's return stack buffer predicts them. A guest call into an OS
import stub is a CALL too, but the stub left through an indirect JMP via the jump table, so its
return-stack entry was never popped. Every OS call (hundreds of thousands a second) left the
predictor one entry off, and the returns after it mispredicted. Now the stub leaves like a guest
`blr`: next guest address in EDX, then RET. Return sites check EDX against their own address, so
a RET that finds a frame the guest isn't returning to falls through to the jump table (Crema Lab
v03.35, md5 55881e70). Any JIT that maps guest calls to host CALLs must leave every called
routine by RET, or it desynchronizes the return predictor.

Measured with v03.34's sampling profile and v03.35's first run (2026-11-15, BotW open world,
profiler on): 25.7-26.4 FPS for 30 minutes against 24.2-24.7 on v03.34. Not isolated: v03.35 also
cut the profiler's own cost (below), and the driver thread stayed on all run.

## Where an emulated core's time goes: BotW's core 2 (measured, v03.34)

Core 2's host thread: 68.5% recompiled game code, 26% Crema's own code, 5.6% system. The game code
is spread over 2,127 functions, but four are 16% of the core: the C++ runtime's array construct
and destruct helpers (a loop calling the element constructor through `bctrl`) and the constructor
and destructor of an 8-byte object (they set two vtable words). The loop's own overhead was twice
the constructor's: guest registers stored to the CPU state before each call and loaded after it.
Next: a byte-compare loop (inlined strcmp). Threads on the core: RadarMgr runs 22% and is ready
76% of the time, the other 15 threads share the rest; yields that find no other thread already
return at once (v03.34), so this is real work.

## Jump encoding on Jaguar: branch relaxation (built, not measured yet)

Upstream Cemu's x86 backend emits every jump between a function's blocks in the 6-byte Jcc (5-byte
JMP) form and, when the target turns out to be near, rewrites it as a 2-byte Jcc followed by a
4-byte NOP. That's the same size, plus one more instruction to decode on the fall-through path, and
Jaguar decodes two per cycle. Crema now generates a function twice: pass 1 as before, and pass 2
emits the jumps that reached within a byte in pass 1 in the true 2-byte form (JMP too, which upstream
never shortened). Pass 2 only removes bytes, and a jump's displacement spans only the code between
it and its target, so every such jump still reaches. A check falls back to pass 1 if one doesn't.
Also: where a block's cycle-count `SUB R12D, n` comes right before its cycle check in the same block,
the check is `JS` on the SUB's flags instead of `BT R12D, 31` + `JC` (jumps only land at block
starts). BotW, 71,962 functions: 97.50 MB of x86 code -> 96.05 MB (-1.5%), compile time unchanged
(code generation is a small part of it). A tight byte loop went from 15 to 13 instructions per
iteration. PC tests: random programs and 3,000 chained runs of BotW's real functions, results
identical with it on and off (Crema Lab v03.36, md5 48f5a9f6).

## Register write-backs inside loops (built, not measured yet)

Cemu's register allocator keeps a guest register in a host register across blocks and stores it
back only where the value leaves its live range. It moves that store from the write to the range's
endings (out of a loop) only if it can account for every successor block. A successor without a
range for the register counted as unknown, so the store stayed at the write, inside the loop. In
BotW's strcmp loop that was a store on every iteration because the value is live after the loop
and flows into code with exits. Two changes:
- A successor that overwrites the register before any read, call or exit needs no value: skipped.
- Any other successor without a range gets the store at the end of the range piece before it, the
  same place the allocator already stores a range that goes on. With such stores the move is made
  only when it is cheaper (out of a deeper loop); at the same depth every path still stores once,
  and the code would only grow.
The loop: 12 instructions per iteration, from 15 before both changes. BotW code +0.14%, from stores
duplicated at several exits. PC tests: random programs in six modes (extra entry points, cut-off
code, 8 registers) identical. Chained BotW runs (two seeds, 14,000 runs): the 7,663 that completed
are identical; 80 that faulted midway differ only in register values not yet stored at the fault,
with the same memory writes (Crema Lab v03.36, md5 48f5a9f6).

## Profiling cost (measured)

Timing every OS call (two rdtsc around each, ~300k calls a second) cost BotW's main core ~11% of
its samples, so profiled runs were slower than normal play. Now a function's first 64 calls in a
report window are timed, then one in 16, which counts for 16 (Crema Lab v03.35). The v03.34 sample
addresses showed it: 11% of core 1 in the 16-byte buckets around the two `rdtsc`.

## How these were found

- The kit's sampler, per thread, with the release ELF kept for every build: `llvm-symbolizer-21`
  names functions. Release builds have no line info, so hot spots inside a function come from
  sample addresses and `llvm-objdump-21 -d`.
- Mangled C++ names need `llvm-nm -C` to find a function's address and size.
- Per-stage timers in the draw path (TSC around each stage, printed every 5 s) give the
  split per draw, which the sampler can't (inlining).
- Timing logs streamed to the PC (Crema Lab Link) give fps, render-thread idle %, and per-stage costs
  for every run without asking for log files.
- JIT samples carry the guest function and the x86 offset in it. The recompiler built for the PC
  (Crema's tests/jittest) compiles the same game function from the game's code and writes its x86,
  so a hot offset can be read as an instruction. It also gives the code bytes per intermediate
  instruction kind over a whole game, which shows where a code-generation change pays.
