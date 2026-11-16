# Switch GPU Triangle Renderer — Stages 0–1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure what MC2's race draws (stage 0, with its stop gate), then draw the race's triangles and sprites with
deko3d's graphics hardware into a GPU frame image shown on screen (stage 1: no blending/depth yet).

**Architecture:** A pure classifier (`hwr/classify.hpp`, PC-tested) decides per shape "GPU or painter" and records
features. Stage 0 counts with it inside `gpu::draw` and the transfer hooks, on the PC GL build. Stage 1 adds a small
graphics API to `backend.hpp` (implemented for deko3d only), a pure vertex converter (PC-tested), a target/texture cache
and an `hwr::draw` path in `gpu.cpp` behind `PS2_HWR=1`, plus a screen dump to check pictures in Ryujinx.

**Tech Stack:** C++20, deko3d + uam (Switch), libnx, PC GL build (llvm-mingw) for counting and unit tests.

**Spec:** `docs/superpowers/specs/2026-11-15-switch-hw-renderer-design.md`

Paths are relative to `MC2Recomp/ps2-native-recomp/` unless they start with `tools/`, `baseline/`, `docs/`.
Stages 2–3 get their own plan after this one (they depend on stage 0's numbers).

## Global Constraints

- Exactness: "looks the same is enough" — no pixel-identical gate; look check = side-by-side + diff vs `baseline/pc-gl`, flag > 2 % of pixels differing by > 24 in any channel (full frame).
- `PS2_HWR=0` (and unset, until stage 3 passes) = painter only, today's picture; the PC 75-image check must stay 0 diff.
- Stop rule after stage 0: painter-needing shapes > 10 % of race bounding-box pixels, or sync traffic implying a full-target write-back/reload every frame → stop and report to the user; no stage 1 without their go-ahead.
- Memory caps: targets 64 MB, texture cache 256 MB (LRU), vertex ring 16 MB per frame in flight; `[hwr-mem]` line every 300 frames.
- Hardware runs only at stage ends; Ryujinx frame time is a CPU proxy only.
- Toolkit v0.0.4 + MC2 patch; nothing is committed unless the user asks (commit steps run only with their go-ahead).

## Review Focus

- A shape with PRMODECONT = 0 takes its TME/ABE/FGE/IIP/FST from PRMODE, not PRIM — the classifier must read the right register (Task 1 test `prmode_selects_attributes`).
- Sprite with u/v (FST=1) vs s/t/q (FST=0) texture coordinates: both convert correctly (Task 4 tests).
- XYOFFSET subtraction and the 12.4 fixed point: a vertex at (XYOFFSET + 16·x) lands on pixel x (Task 4 test).
- Context 2 (PRIM.CTXT = 1) uses ctx[1]'s FRAME/TEST/ALPHA/TEX0 (Task 1 test `context_two`).
- A texture whose TBP equals a cached target's FBP (render-to-texture) is counted as "from target", not "from EE" (Task 2 counter + Task 1 helper test).

---

### Task 1: Shape classifier (pure, PC-tested)

**Files:**
- Create: `runtime/src/gs/gpu/hwr/classify.hpp`
- Create: `tests/hwr_classify_test.cpp`

**Interfaces:**
- Produces (namespace `ps2::gs::hwr`):
  - `enum Prim : std::uint8_t { kPoint, kLine, kLineStrip, kTri, kTriStrip, kTriFan, kSprite };`
  - `enum Painter : std::uint32_t` bits: `kPainterPointLine = 1, kPainterDestAlphaBlend = 2 (ALPHA.C == 1), kPainterDestAlphaTest = 4 (TEST.DATE), kPainterPartialMask = 8 (FBMSK != 0 && != 0xFFFFFFFF), kPainterFrameFormat = 16 (frame PSM not CT32/CT24/CT16/CT16S), kPainterWrapBlend = 32 (ABE && COLCLAMP == 0), kPainterFba = 64 (FBA)`
  - `enum Feature : std::uint32_t` bits: `kTextured = 1, kBlend = 2, kAlphaTest = 4, kDepthTest = 8, kDepthWrite = 16, kFog = 32, kMip = 64 (TEX1.MXL > 0 and MMIN >= 2), kBilinear = 128, kStq = 256 (!FST), kRegionWrap = 512 (WMS or WMT >= 2), kPalette = 1024 (texture PSM T8/T4/T8H/T4HL/T4HH)`
  - `struct ShapeClass { Prim prim; std::uint32_t painter; std::uint32_t features; std::uint32_t fbp, fbw, fpsm, zbp, tbp, tpsm; };`
  - `ShapeClass classify(const ps2::gs::State &s)` — reads PRIM (type, CTXT) and, when `(s.prmodecont & 1) == 0`, TME/FGE/ABE/IIP/FST from PRMODE; context = `s.ctx[ctxt]`; GS register bit layout per the GS manual (TEST: ATE 0, DATE 14, ZTE 16, ZTST 17–18; ALPHA: C bits 4–5; FRAME: FBP 0–8, FBW 16–21, PSM 24–29, FBMSK 32–63; ZBUF: ZBP 0–8, ZMSK 32; TEX0: TBP0 0–13, PSM 20–25; TEX1: MXL 2–4, MMAG 5, MMIN 6–8; CLAMP: WMS 0–1, WMT 2–3). Depth write = ZTE && !ZMSK; depth test = ZTE && ZTST != 1 (ALWAYS).
  - `bool texture_from_target(const ShapeClass &c, std::uint32_t target_fbp)` — `kTextured` and `c.tbp == target_fbp * 32` (TBP0 is in 64-word blocks, FBP in 2048-word pages: ×32).

- [ ] **Step 1: Write the failing test** `tests/hwr_classify_test.cpp` (plain asserts, builds a `State` by setting the 64-bit registers):
  - `plain_textured_triangle`: PRIM = tri(3) | TME(1<<4); PRMODECONT = 1 → `prim == kTri`, `features & kTextured`, `painter == 0`.
  - `prmode_selects_attributes`: PRMODECONT = 0, PRIM = tri with TME clear, PRMODE has TME set → `features & kTextured`.
  - `context_two`: PRIM CTXT bit (1<<9) set, ctx[1].frame FBP = 5, ctx[0].frame FBP = 1 → `fbp == 5`.
  - `line_goes_to_painter`: PRIM = line(1) → `painter & kPainterPointLine`.
  - `dest_alpha_blend`: ABE set, ctx ALPHA C = 1 (`1 << 4`) → `painter & kPainterDestAlphaBlend`.
  - `partial_mask`: FBMSK = 0x00FF0000 → `kPainterPartialMask`; FBMSK = 0 and 0xFFFFFFFF → not set.
  - `region_wrap_and_stq`: CLAMP WMS = 2, FST clear → `kRegionWrap | kStq`.
  - `texture_from_target`: TBP0 = 160, FBP = 5 → true; TBP0 = 161 → false.

- [ ] **Step 2: Run it to see it fail** — `clang++ -std=c++20 -I runtime/src -I runtime/include tests/hwr_classify_test.cpp -o /tmp/hwr_classify.exe` → Expected: `classify.hpp` not found. (If `gs.hpp` pulls in more headers than compile standalone, the test defines a minimal `State` copy only if needed and the ledger records it.)

- [ ] **Step 3: Implement `classify.hpp`** (header-only, no allocation).

- [ ] **Step 4: Run the test** — Expected: exit 0.

- [ ] **Step 5: Commit** (with go-ahead): `hwr: shape classifier for the GPU renderer (stage 0)`.

### Task 2: Stage 0 counters and the stop-rule decision

**Files:**
- Create: `runtime/src/gs/gpu/hwr/stats.hpp`, `runtime/src/gs/gpu/hwr/stats.cpp`
- Modify: `runtime/src/gs/gpu/gpu.cpp` (`draw`, `image_data`, `local_copy`, `download`, `download_async`, `display`/`present`: one call each, behind `PS2_HWR_STATS=1`)
- Modify: `CMakeLists.txt` (add `stats.cpp` to the GL source list next to `gpu.cpp`, both GL and deko3d branches)
- Modify: `HANDOFF.md`

**Interfaces:**
- Consumes: Task 1 `classify`, `texture_from_target`.
- Produces: `void hwr::stats_shape(const ShapeClass &c, std::uint32_t box_pixels)`; `void hwr::stats_sync(const char *kind, std::uint32_t gs_address, std::uint32_t bytes)`; `void hwr::stats_frame()` (prints every 300 frames via `ps2::frames_run()`): `[hwr-stats] frames to N: shapes S (box px P) | painter shapes s (box px p, p/P %) by reason ... | features ... | textures: from EE e, from target t | sync per frame: download d (B bytes), local_copy l, upload-into-target u, display-non-target x`. A "target" for stage 0 = any (FBP, FBW, FPSM) drawn to in the last 300 frames.

- [ ] **Step 1: Implement** the counters (box pixels from the shape's vertex bounding box after XYOFFSET, clipped to the scissor).
- [ ] **Step 2: PC GL build + harness** with `PS2_HWR_STATS=1` exported. Expected: `[hwr-stats]` lines in `harness.log`; compare.py still 75/75 `OK diff=0/` (counters only).
- [ ] **Step 3: Decide the gate** from the race lines (frames 3900–4500): painter share of box pixels and sync traffic. Write the numbers, the fallback list and the in-shader-blend need into `HANDOFF.md`. If the stop rule trips: stop, report to the user, end this plan here.
- [ ] **Step 4: Commit** (with go-ahead): `hwr: stage 0 counters (PS2_HWR_STATS)`.

### Task 3: deko3d graphics API for the renderer + self-test

**Files:**
- Modify: `runtime/src/gs/gpu/backend.hpp`, `runtime/src/gs/gpu/backend_deko3d.cpp`, `runtime/src/gs/gpu/backend_gl.cpp` (stubs returning false/empty)
- Create: `runtime/src/gs/gpu/dk/hwr.vert`, `runtime/src/gs/gpu/dk/hwr.frag` (stage 1: position + colour + optional texture; uber state in UBO binding 0, texture binding 0)
- Modify: `CMakeLists.txt` (two more entries in the deko3d shader list)

**Interfaces:**
- Produces (namespace `ps2::gs::gpu::be`):
  - `struct Image { std::uint32_t id = 0; explicit operator bool() const { return id != 0; } };`
  - `Image create_image(std::uint32_t w, std::uint32_t h, bool render_target);` (RGBA8), `void destroy_image(Image &);`
  - `void upload_image_from_buffer(Buffer src, std::size_t offset, Image dst, std::uint32_t w, std::uint32_t h);` (ordered in the command stream, like `copy_buffer`)
  - `struct HwrVertex { float x, y, z, w; std::uint32_t rgba; float s, t, q; };` (screen pixels; the vertex shader maps to clip space with the target size from the UBO)
  - `struct HwrDraw { Image target; Image texture; std::uint32_t scissor[4]; std::uint32_t first, count; std::uint32_t state[16]; };`
  - `bool hwr_draw(const HwrVertex *verts, std::uint32_t nverts, const HwrDraw *draws, std::uint32_t ndraws);` (vertices into a 16 MB ring, per frame in flight)
  - `bool present_image(Image src, const PresentInfo &info);` (blit an image to the swapchain instead of the scratch buffer)
  - `bool read_image(Image src, std::uint32_t *rgba, std::uint32_t w, std::uint32_t h);` (waits; for dumps and tests)
- Env: `PS2_HWR_SELFTEST=1` → at init, draw one red triangle covering the left half of a 64×64 cleared-black image, read it back, log `[hwr] self-test: ok` or `[hwr] self-test: FAILED (pixel (x,y) = 0x...)`.

- [ ] **Step 1: Self-test first** (the failing check): add the `PS2_HWR_SELFTEST` code path calling the API; build. Expected: link errors / missing functions.
- [ ] **Step 2: Implement** the API in `backend_deko3d.cpp` (images = `DkImage` with `UsageRender` for targets, texture descriptors for sampling, vertex buffer attribute state matching `HwrVertex`).
- [ ] **Step 3: Ryujinx run** with `PS2_HWR_SELFTEST=1`, `--frames 60`. Expected: `[hwr] self-test: ok`, game runs on.
- [ ] **Step 4: Commit** (with go-ahead): `deko3d: image + triangle drawing API for the GPU renderer`.

### Task 4: Vertex conversion (pure, PC-tested)

**Files:**
- Create: `runtime/src/gs/gpu/hwr/vertices.hpp`
- Create: `tests/hwr_vertices_test.cpp`

**Interfaces:**
- Consumes: `HwrVertex` layout from Task 3 (redeclared identically in `vertices.hpp` if `backend.hpp` cannot be included on the PC test; the ledger records it).
- Produces: `std::uint32_t hwr::convert_shape(const ps2::gs::State &s, const ShapeClass &c, const ps2::gs::Vertex *v, int count, HwrVertex *out)` — returns vertices written (triangle list): tri 3; sprite 6 (two triangles from the two corners, colour/Z/texcoords of the second vertex as the GS does); strips/fans arrive from `gs.cpp` already as one triangle per call (count 3) — assert that in the test. Position: `(x − XYOFFSET.OFX) / 16`, same for y, as float; z as float `z` (stage 2 scales it); colour packed RGBA8; texture coords: FST=1 → u/16, v/16 in texels with q = 1; FST=0 → s, t, q as given (the fragment shader divides).

- [ ] **Step 1: Write the failing test** `tests/hwr_vertices_test.cpp`:
  - `tri_positions`: XYOFFSET OFX = 2048·16 (as GS stores ×16), vertex x = (2048 + 10)·16 → out x == 10.0f.
  - `sprite_two_triangles`: count 2 corners (0,0)-(16,8) → 6 vertices covering both corners; every vertex has the second vertex's colour.
  - `fst_uv`: FST=1, u = 32 (2 texels ×16) → s == 2.0f, q == 1.0f.
  - `stq_passthrough`: FST=0, s=0.5, t=0.25, q=2 → same values.
- [ ] **Step 2: Run** (`clang++ -std=c++20 -I runtime/src -I runtime/include tests/hwr_vertices_test.cpp`) → fails (missing header).
- [ ] **Step 3: Implement** `vertices.hpp`.
- [ ] **Step 4: Run** → exit 0.
- [ ] **Step 5: Commit** (with go-ahead): `hwr: GS vertices to GPU triangles`.

### Task 5: Stage 1 — race geometry on the GPU, shown on screen

**Files:**
- Create: `runtime/src/gs/gpu/hwr/renderer.hpp`, `runtime/src/gs/gpu/hwr/renderer.cpp` (target cache keyed by (FBP, FBW, FPSM); texture cache keyed by (TBP, TBW, TPSM, TW, TH, CBP, CPSM, CSA) using the painter's existing decode path into a buffer + `upload_image_from_buffer`, invalidated by `image_data`/`local_copy` overlap; LRU caps from Global Constraints; `[hwr-mem]` line)
- Modify: `runtime/src/gs/gpu/gpu.cpp` (`draw`: when `PS2_HWR=1` and `classify(...).painter == 0` → `hwr::draw`, else (stage 1 only) drop the shape; `present`: if DISPFB matches a cached target → `be::present_image`)
- Modify: `runtime/src/gs/gpu/backend_deko3d.cpp` (`PS2_DK_SCREEN_DUMP=<file>@<picture>`: PPM of the swapchain image, top row first)
- Modify: `CMakeLists.txt` (renderer.cpp in the deko3d branch only)

**Interfaces:**
- Consumes: Tasks 1, 3, 4.
- Produces: `void hwr::draw(const ps2::gs::State &s, const ps2::gs::Vertex *v, int count);` `bool hwr::present(const DisplaySource &src);` `void hwr::invalidate(std::uint32_t gs_address, std::uint32_t bytes);` `void hwr::frame_end();`

- [ ] **Step 1: Implement** (no blending, no depth, no alpha test in stage 1; untextured = vertex colour, textured = texture × vertex colour, TFX/TCC ignored).
- [ ] **Step 2: Switch build**; painter path unchanged: PC 75-image check still 0 diff (GL build does not compile the renderer).
- [ ] **Step 3: Ryujinx run**, harness inputs to 3700, `PS2_HWR=1`, `PS2_DK_SCREEN_DUMP=/mc2/screen.ppm@<race frame>`. Expected: no crash; the race geometry (road, buildings, sky) appears with the right shapes and textures in the dump (by eye vs `baseline/pc-gl/04_race_start.png`; blending/depth errors expected at this stage); `[hwr-mem]` flat.
- [ ] **Step 4: Hardware run** (stage end; user's netloader press), `PS2_HWR=1`, `PS2_DK_STATS=1`, `PS2_IOP_TIME=1`: race ms/frame and CPU busy recorded in `HANDOFF.md` (an early signal whether the CPU side holds, since stage 1 already does the full per-shape CPU work).
- [ ] **Step 5: Commit** (with go-ahead): `hwr: stage 1 — GS triangles drawn by deko3d into GPU targets (PS2_HWR=1)`.
