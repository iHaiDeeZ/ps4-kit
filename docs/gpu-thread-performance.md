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
  triggers. Auto's console result is not measured yet (Crema Lab 109ee892 and later).

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

## How these were found

- The kit's sampler, per thread, with the release ELF kept for every build: `llvm-symbolizer-21`
  names functions. Release builds have no line info, so hot spots inside a function come from
  sample addresses and `llvm-objdump-21 -d`.
- Mangled C++ names need `llvm-nm -C` to find a function's address and size.
- Per-stage timers in the draw path (TSC around each stage, printed every 5 s) give the
  split per draw, which the sampler can't (inlining).
- Timing logs streamed to the PC (Crema Lab Link) give fps, render-thread idle %, and per-stage costs
  for every run without asking for log files.
