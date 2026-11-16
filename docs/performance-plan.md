# MC2 on Switch: performance to-dos toward full speed (60 fields/s)

## Context

Port runs correctly on hardware (deko3d + GPU triangle renderer `PS2_HWR=1`, VU1 compiled ahead of time, 149 programs).
Handheld, CPU 1785 MHz. Last measurements:

| Race | Wall per emulated frame | Target |
|---|---|---|
| Los Angeles | 19.7–20.1 ms (~50/s, 83 % speed) | 16.7 ms |
| Tokyo (before batch fix) | 22–28 ms | 16.7 ms |

Three pipelined threads, each on its own core, all close to the limit (LA, per frame):

| Thread | Busy | Known parts |
|---|---|---|
| Emulation (core 0) | 17.2 ms | IOP 3.5 ms; waits for pictures 3 ms; rest = game code + sync |
| Vector units (core 2) | 13–14 ms (peaks 19.8) | VU1 programs 7.7 ms; VIF unpack/other ~5 ms |
| Render / GS (core 1) | 10.5–15.7 ms | `draw()` 12 ms, of it texture fetches 5.8 ms |

No single bottleneck left → every thread needs ~20–30 % off. Old plan (bring-up phases 0–4) is done; this replaces it.

Working rules from the user: think and read first, iterate on PC, few NRO pushes, tests only once planning for a batch is exhausted.

## Method

- **PC first.** PC runner (`build/native-mc2`, GL build `build/gl-mc2-o3`) for: correctness (74 harness images, `tools/compare.py`
  vs `baseline/pc-gl`), deterministic counters (texture fetches, batch splits, IOP instructions, VIF command counts, shapes),
  and relative profiles (`cost::Timer` report, `PS2_IOP_HIST`, per-program VU time). Counters transfer 1:1 to the Switch; only
  absolute ms do not.
- **One NRO per batch**, three batches (A, B, C below) → 3 pushes total, each one Tokyo run from `arcade.state` with
  `PS2_CORES=1` (heaviest scene, shows all three threads + waits). LA picture check stays on PC.
- Each item: read code → decide → implement → PC check. No Switch run per item.
- Nothing committed unless asked.

## Batch A — largest known wins

### A1. Texture cache keyed by content (render thread, −5 to −6 ms, fewer GPU dispatches)
- Today: `texture_for()` in `runtime/src/gs/gpu/hwr/renderer.cpp` keys on `{tex0, texa}` and refetches when the pages'
  generation changed. Game re-uploads textures into the same GS memory areas every frame → ~239 fetches + 18.7 refetch
  batch splits per frame, each a GPU decode (`hwr_fetch_texture`, `gpu.cpp:4055`) plus `load_clut`.
- Change: hash the bytes of each upload (transfer data is on the CPU when it is queued; ARM `crc32` instructions, PC
  `_mm_crc32`). Pages carry the hash of what was last uploaded instead of only a generation. Texture key =
  `{tex0 sans TCC/TFX/CLD, texa, content hash of texel pages, content hash of CLUT pages}`. Same content → same GPU image, no
  fetch, no flush. Pages drawn by the GPU (`kDrawn`) keep the generation path.
- Reuse: `touch_rect`, `max_gen_rect`, `g_page_owner`, eviction by `last_used`.
- PC check: fetches per frame at steady state → near 0; 74 images unchanged.
- Risk: partial uploads over a texture (hash must combine all covering uploads; fall back to generation when a page has
  mixed owners).

### A2. IOP polling loop (emulation thread, up to −3.5 ms)
- Fact: IOP runs its full 615 k cycles every frame although translated regions are in the build. `games/mc2/iop/capture.txt`
  shows one loop at `0x0008B0B0–0x0008B1F0` (and `0x00093714/24`) executing ~equally often: thread 41, priority 123, always
  runnable → idle skip in `iop::run` (`kernel.cpp:662`) never triggers.
- To do (PC): disassemble the loop from `capture.txt.ram`; find what it polls. If it only reads memory/registers that change
  solely through events or interrupts: add a skip like the EE's `spin_watch` (`runtime/src/hw.cpp:201`) — when that thread is
  the only runnable one and an iteration changed nothing, advance `g_now` to the next event.
- PC check: `PS2_IOP_TIME=1` instructions per frame drop; audio and loading unchanged; images unchanged.

### A3. Verify the batch-split fix in Tokyo
- No work; measured by batch A's Switch run (expect ~640 → few target-read splits, picture waits 8–11 ms → small).

## Batch B — vector-unit thread (13–20 ms → ≤ 11 ms)

### B1. VU1 program cost, top programs first
- PC: per-program time/calls table (extend the `ChainTime` guard in `vu_micro.cpp` to bucket by `program_key`, env-gated).
- Read the generated C++ of the top 3–5 (`generated/mc2/vu1_*.cpp`) for avoidable work: status/MAC/clip flags computed but
  never read, `SSE2NEON_PRECISE_MINMAX/SQRT` (`CMakeLists.txt:93`) on paths that do not need it, per-instruction memory
  bounds masks, DIV/RSQRT emulation. Fix in `recomp/vurecomp_gen.cpp` (generator), regenerate.
- Build flags for generated VU units: `-O2` today → try `-O3 -fno-math-errno -fno-trapping-math` (not `-ffast-math`: results
  must stay bit-equal).
- PC check: 74 images 0 diff (bit-equal VU output is the gate).

### B2. VIF1 unpack (~5 ms)
- PC: count unpack commands by format/mask/mode for the race (deterministic). `runtime/src/vif.cpp` (683 lines, 20 intrinsics).
- Add straight-line fast paths for the top formats when mask is off and mode is 0 (whole-run conversion instead of per
  vector), using the existing SSE source (sse2neon on Switch).

### B3. Path-1 packet hand-off
- Read how XGKICK data reaches the render queue (`system.cpp` `run_vu_thread`, `gs::queue`): one copy only, no per-packet
  allocation or lock. Fix only if reading shows waste.

## Batch C — emulation thread rest and render thread rest

### C1. Emulation thread breakdown (PC, existing `cost::Timer`)
- Run the race with the cost report → shares of game code, `sync_iop`, SIF pump, DMA/VIF queueing, SPU2 mixing, events.
- Act on the top non-game item. Candidates to check by reading: SPU2 mixing on this thread (move the mix to the audio
  callback thread on core 2/1 if it is here), DMA chain copies to the VU queue (copy once), slice length (`kMaxSlice`).

### C2. Generated EE code and build flags
- Read one hot generated function (`generated/mc2/unit_*.cpp`) for the memory-access pattern: per-access range checks vs
  direct `g_ram` indexing, cycle accounting per block.
- Flags: whole build is `-O2` without LTO (`build/switch-gl-mc2/CMakeCache.txt`). Try `-O3` for runtime, LTO for
  runtime ↔ generated calls (memory helpers, FPU helpers) if they are out of line. Size/compile time permitting.
- Only if C1 shows game code dominating: list top functions (PC sampling profile `host/profile_win.cpp`), look for
  hand-replaceable routines (memcpy/memset/matrix loops) → native versions via the existing patch/override mechanism.

### C3. Render thread remainder (~6 ms draw + ~4 ms outside)
- After A1, re-read `draw_shape` (`renderer.cpp`), `vertices.hpp`, `classify.hpp`: per-shape work that can be per-state
  (state hashing, blend classification, scissor) → cache on GS state change, not per shape.
- GS register decode on this thread: check batching of GIF packed data (`gs.cpp`).
- Upload path: 28+ `upload` dispatches and `load_clut` per frame — after A1 `load_clut` is needed only for the compute
  path; skip it while `PS2_HWR` is active if nothing else reads it.

### C4. Pacing (last)
- Once all threads < 16 ms: present on vsync without blocking the emulation thread (keep ≤ 2 pictures in flight), throttle
  to 59.94 Hz, check audio stretching disengages (`[audio] stretching` line).

## Not planned (reasons)
- Pre-unpacking the ISO / shader precompile: load time only, not frame time. Shaders are already built offline (`uam`).
- More GPU work: GPU is no longer the limit with the triangle renderer.
- 4th core: not available to applications.
- Split-screen pad port 2: correctness, separate task.

## Critical files
- `runtime/src/gs/gpu/hwr/renderer.cpp`, `pages.hpp`, `runtime/src/gs/gpu/gpu.cpp` (A1, C3)
- `runtime/src/iop/kernel.cpp`, `iop/cpu.cpp`, `scripts/iop_recomp.py`, `games/mc2/iop/` (A2)
- `runtime/src/vu_micro.cpp`, `recomp/vurecomp_gen.cpp`, `runtime/include/ps2/vu_ops.hpp`, `runtime/src/vif.cpp` (B)
- `runtime/src/system.cpp`, `runtime/src/iop/spu2.cpp`, `CMakeLists.txt`, `generated/mc2/` (C)

## Verification
- Per item (PC): 74 harness images vs `baseline/pc-gl` = 0 diff; the item's counter moved as predicted.
- Per batch (Switch, one push): Tokyo from `arcade.state`, `PS2_CORES=1 PS2_HWR_LOG=1 PS2_DK_STATS=1 PS2_IOP_TIME=1`;
  read `[busy]` per thread, waits, frame wall; one screenshot vs the previous batch.
- Done: Tokyo and LA ≤ 16.7 ms per frame, audio not stretched. Results into `HANDOFF.md` after each batch.

## Open point to settle while executing A1
- Whether `PS2_HWR` runs on the PC GL backend (it goes through `backend.hpp`). If not, A1/C3 picture checks need the
  Switch screenshot, and making hwr run on PC GL becomes the first to-do (it pays for itself in saved pushes).
