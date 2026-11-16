# Switch GPU Triangle Renderer — Stages 2–3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the GPU renderer's race "look the same" as the PC painter (blending, depth, alpha test, fog, texture
functions; stage 2), then run the whole harness through it with the painter as fallback and measure on hardware (stage 3).

**Architecture:** One uber fragment shader (`dk/hwr.frag`) with GS state in its uniform block and **dual-source blending**
(output 0 = colour with the GS source alpha, output 1 alpha = the blend factor), so every common GS blend
`(A−B)·C/128 + D` maps to a fixed-function blend with factors Src1Alpha/InvSrc1Alpha/One/Zero and op Add/ReverseSubtract,
and alpha-test failures that keep colour become factor 0. Depth: one D32F image per (ZBP, target size), GS Z scaled by
the Z format's range. Stage 3 adds the painter fallback for shapes `classify()` sends there (write-back of the touched
target region to GS memory with a compute pass, painter draw, reload) and the remaining sync paths.

**Tech Stack:** deko3d + uam (Switch), Ryujinx for pictures, the user's Switch for timing.

**Spec:** `docs/superpowers/specs/2026-11-15-switch-hw-renderer-design.md`. Stage 0–1 results: ledger
`docs/superpowers/plans/2026-11-15-switch-hw-renderer-stage0-1.progress.md`.

## Global Constraints

- Exactness: "looks the same" (see spec); check = Ryujinx screen dumps (`PS2_DK_SCREEN_DUMP=<file>@<picture>`) side by side with `baseline/pc-gl`, metric: share of pixels differing by > 24 in any channel, > 2 % → look by eye.
- deko3d shaders: single exit, no `return`/early exit in `main` (Mesa emits PRET, Ryujinx cannot run it) — `discard` is allowed.
- `PS2_HWR=0`/unset = painter only, PC 75-image check stays 0 diff.
- Memory caps (targets 64 MB, textures 256 MB LRU), hardware runs only at stage ends.

## Review Focus

- AFAIL = ZB_ONLY / FB_ONLY / RGB_ONLY with a failing alpha test (Task 2 cases in the GS-state mapping test).
- Blend with C = FIX > 128 (factor above 1) — clamped to 1 (documented approximation).
- ZTST NEVER (draw nothing) and ZTE with ZMSK (test without write).
- A target used as a texture while being drawn into (feedback) — must not sample the image being rendered (copy first).
- Texture fetch per frame on hardware (~218 submits/frame in Ryujinx) — measure; batch fetches into one submit if costly.

---

### Task 1: GS state → GPU state mapping (pure, PC-tested)

**Files:** Create `runtime/src/gs/gpu/hwr/state.hpp`, `tests/hwr_state_test.cpp`.

**Interfaces:**
- Produces `struct GpuState { std::uint32_t shader[8]; std::uint8_t blend, ztest; bool zwrite; std::uint32_t zbp, zpsm; };` and
  `GpuState map_state(const ps2::gs::State &s, const ShapeClass &c)`:
  - `shader[0]` bits: 0 textured, 1 FST, 2 bilinear, 3 clamp, 4–5 TFX, 6 TCC, 7 fog, 8 alpha test on, 9–11 ATST, 12–13 AFAIL, 14 blend factor from FIX (else As), 15 no-blend (factor 1);
    `shader[1]` = tw | th<<16; `shader[2]` = AREF | FIX<<8; `shader[3]` = FOGCOL; `shader[4]` = float bits of 1/Zmax (Z32 2^32, Z24 2^24, Z16 2^16).
  - `blend`: 0 lerp (Src1Alpha, InvSrc1Alpha, Add), 1 add (Src1Alpha, One, Add), 2 reverse-subtract (Src1Alpha, One, RevSubtract), 3 source only (Src1Alpha, Zero), 4 unmapped (counted, drawn as lerp).
    (A,B,C,D) = (Cs,Cd,C,Cd)→0, (Cs,0,C,Cd)→1, (0,Cs,C,Cd)→2, (Cs,0,C,0)→3; ABE off → 0 with shader bit 15.
  - `ztest`: 0 off, 1 never, 2 always, 3 gequal, 4 greater (ZTE, ZTST); `zwrite` = ZTE && !ZMSK.
- [ ] Step 1: failing test `tests/hwr_state_test.cpp` with one case per blend row above, ABE off, ZTST each value, ZMSK, AFAIL each value, FIX 0x80 → shader[2] == AREF | 0x80<<8, zscale for Z24 == 1/16777216.
- [ ] Step 2: run → fails (no header). Step 3: implement. Step 4: run → `hwr_state_test: ok`.

### Task 2: Uber shaders + dual-source blending + depth in the backend

**Files:** Modify `dk/hwr.vert` (z = z·zscale·2−1, fog from `a_pos.w`), `dk/hwr.frag` (TFX/TCC per GS manual in 0..255 units,
fog `lerp(FOGCOL, c, f/255)`, alpha test with AFAIL: KEEP→discard, ZB_ONLY→factor 0, FB_ONLY/RGB_ONLY→pass; output 0 =
(rgb, As)/255, `layout(location = 0, index = 1)` output alpha = factor), `backend.hpp` (HwrDraw gains `Image depth; std::uint8_t blend, ztest; bool zwrite;`,
`Image create_depth_image(w, h)`), `backend_deko3d.cpp` (D32F images, `DkDepthStencilState`, blend state per draw, depth clear to 0),
`hwr/vertices.hpp` (fog into `w`; update `tests/hwr_vertices_test.cpp`: `fog_in_w`).
- [ ] Self-test extended (`PS2_HWR_SELFTEST=2`): two overlapping triangles with lerp blend 50 % → pixel = mix; depth GREATER rejects the farther one. Ryujinx: `self-test: ok`.

### Task 3: Renderer uses the mapping; race look check

**Files:** `hwr/renderer.cpp` (map_state per shape; depth image per (ZBP, target); batch key includes blend/depth),
`HANDOFF.md`.
- [ ] Ryujinx run to 3700 with dumps at pictures 300, 600, 1800, 3500: side-by-side + metric vs baseline; record in HANDOFF.
- [ ] Fix the largest visible differences (one ledger line each).

### Task 4: Stage 3 — painter fallback, feedback, sync

**Files:** `hwr/renderer.cpp`, `gpu.cpp`, new compute `dk/hwr_writeback.comp` (target image → GS memory in its PSM, using the
painter's address functions via the lean script, like texfetch), `backend_deko3d.cpp` (image → buffer copies).
- Painter shapes: flush batch, write back the target region they touch (bounding box) to GS memory, let the painter draw
  them (normal gpu.cpp path), mark the target region stale; reload stale regions into the target image before the next GPU
  draw (texfetch-like compute from GS memory into the image).
- Feedback (texture = the target being drawn): copy the region to a scratch image first.
- `download`/`local_copy`/`read_state`: write back the targets overlapping the range first.
- [ ] Ryujinx: full harness, menus (partial FBMSK shapes: map byte-aligned FBMSK to a colour write mask instead of the painter), `[hwr-mem]` flat.

### Task 5: Hardware measurement (stage end)

- [ ] Hardware run (user's netloader): `PS2_HWR=1`, harness inputs to 3700, `PS2_DK_STATS=1`, `PS2_IOP_TIME=1`; race ms/frame, CPU busy, GPU blocked; compare with the painter (75–78 ms). Record in HANDOFF; if CPU-bound, profile per-shape CPU cost (batching).
