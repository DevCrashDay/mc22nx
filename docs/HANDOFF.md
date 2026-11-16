# Handoff: Midnight Club II → Nintendo Switch (state of 2026-11-15)

Read `plan.md` (goal, approach) and `to-do.md` (checklist + findings) first. This file is the
current state and what to do next. Nothing is committed: all changes are uncommitted in the git checkout
`MC2Recomp/ps2-native-recomp` (submodule of `MC2Recomp`, base commit 830c7c3 + `MC2Recomp/patches/ps2-native-recomp.patch`).

## Batch A (2026-11-16): Tokyo race 22 ms per frame, render thread is the limit — start here

Plan with all to-dos: `docs/performance-plan.md` (batches A/B/C, PC first, one NRO per batch).

- **GPU renderer on PC**: `PS2_HWR=1` now also works in the PC OpenGL build (`backend_gl.cpp` images/FBOs/dual-source blending,
  `PS2_GPU_HWR` define, CMake embeds `dk/hwr.*` + texfetch for GL). Counters match the Switch exactly → check renderer changes on PC.
  `PS2_GL_SCREEN_DUMP=<file %u>@a,b,c` takes a list. LA harness on PC: `--window` + `args_la.txt`, compare with `cmp` (byte-exact).
- **Batch split fix** (`batch_writes`): Tokyo render thread blocked on the GPU 1.5–1.8 s → 0.7 s per 5 s.
- **IOP idle loop** (`runtime/src/iop/cpu.cpp`, env `PS2_IOP_IDLE=8B048:8B048=8E830000,8B20C=1440FF8E,946B4=0,946CC=0,946D4=0,9C8F4!2`):
  MC2's lowest-priority IOP thread busy-polls 3 request counts; at the loop head with all zero it gives up its slice until the next
  event. IOP 3.8 → 2.2 ms per frame on Switch. Needs the env line (in `env.txt` on the SD card).
- **Texture cache by content** (`hwr/renderer.cpp`): uploads are hashed (`ContentHash`, gpu.cpp `image_data`), pages keep the list of
  uploads over them; `upload_key` names a texture by the upload that wrote its GS memory blocks (8/4-bit textures are uploaded as
  32-bit: GS block tables per format), palettes by what the fetch's palette buffer holds (`clut_loaded`, 16 parts). Images are reused
  (evicted pool, or the least recently used texture of the same size) — creating/destroying deko3d images costs ms each.
  Tokyo fetches 139 → 33 per frame, LA 239 → 16; PC LA pictures byte-identical except one 14×16 spot where the old cache was stale.
- **Switch Tokyo now** (handheld, `arcade.state`): 22 ms per frame; emulation 12.1, vector units 11.5, **render 17 ms CPU** + ~3 ms
  blocked on the GPU. Render: `draw()` 11.9 (per-shape 9.3 — was 6.0 before the content keys: two map lookups + signatures per
  shape; fetches 2.2), upload page tracking 1.2, hashing 0.5, ~4 outside `draw()`.
- **Next**: render thread — memo the per-shape texture lookup (runs of shapes share TEX0), cheaper `upload_done` (block sets),
  then GS decode outside `draw()`; GPU time with `PS2_DK_TIMING=1`. Then batch B (VU) / C as in the plan.

## Where it stands

| Target | State |
|---|---|
| PC, OpenGL backend (`build/gl-mc2-o3`) | **Works.** `tools/compare.py` vs `baseline/pc-gl` = `0/228864 (0.000%)` across all 74 harness PNGs (`<= 3 px` / `0.001%` vs `baseline/pc-wgpu`). |
| PC, WebGPU/D3D12 backend (`build/native-mc2`) | **Works.** Links cleanly; unchanged behavior. |
| Ryujinx 1.3.3, Switch OpenGL build | Runs 4,500 frames without a crash, renders correctly (frame 300 identical to PC). Slow: films 25–90 ms/frame, ESRB/menu 270–380 ms/frame. |
| Real Switch (`NV120`, Mesa `20.1.0-rc3`, title-override launch) | **Correctness COMPLETE (0 hangs across 4,500 frames); GPU-bound.** Logos `0.000%` diff, in-game race `0.187%` diff vs `baseline/pc-gl`. Speed: FMV `13.3–17.7 fps` (~75 ms/frame), in-game race `~1.0–1.3 fps` (`717–1033 ms/frame` wall with periodic PPM dumps). → Next: **Phase 3 (`deko3d`)**. |

## Race at ~48 fps of emulated time: VU1 programs compiled ahead of time (2026-11-16) — start here

Hardware, handheld, CPU 1785 / GPU 921 / memory 1600 MHz, `PS2_HWR=1`, deko3d, race harness frames 3300–3700:

| | before | after |
|---|---|---|
| race wall per emulated frame | 55–58 ms | **20.4–20.9 ms** |
| VU thread (VU1 programs) | 57–60 ms (50–53), 2,100–2,500 interpreted starts/frame | 13–14.5 ms (7–8), **0 interpreted** |
| GS thread / emulation thread | ~15 / ~16 ms (mostly waiting) | ~14–17 / ~17 ms |

Race picture at frame 3600: identical to the earlier `hw_3600.ppm` (0.0 % pixels differ).

- **Cause**: the game's VU1 set (`games/mc2/vu1`) had only 5 recordings (58 functions with jump targets); the race's programs at
  0x0050, 0x2EC8–0x2F58 etc. ran in the interpreter. Found with the new `PS2_CORES=1` Switch report (`[busy]` per thread + VU
  thread split: VIF / VU1 programs / GIF / interpreted starts) — `runtime/src/system.cpp` `core_sample()`, `vu_micro.cpp` `program_ns()`.
- **Fix**: PC run of the same inputs with `PS2_VU_RECORD=<dir> PS2_LIVE=0` (26 recordings, one key each), copied into
  `games/mc2/vu1` (31 now), `build/tools/vurecomp.exe games/mc2/vu1 generated/mc2 --vu0-dir games/mc2/vu0` → 102 functions.
- **Coverage is the lever**: any city/mode/car not in this harness may still hit interpreted VU1 programs. Record a broad PC play
  session with `PS2_VU_RECORD` (and `PS2_VU_COUNT=1` to see misses), add them, regenerate. Switch `[busy]` lines show
  `interpreted starts` per frame.
- Other findings of this round: race has **no** GS→EE downloads (`[hwr]   downloads per present`, `PS2_HWR_DOWNLOAD_SKIP_FROM`);
  GPU-renderer CPU work ~12 ms per picture (139 "target read" batch splits); the old "1,800 waits" were the timing build's own.
- Building: `cmake` from Git-bash fails (TMP / `/opt/devkitpro` mapping). Use devkitPro's shell:
  `C:/devkitPro/msys2/usr/bin/bash.exe -lc 'export TMP=/tmp TEMP=/tmp; cd .../ps2-native-recomp && cmake --build build/switch-gl-mc2 -j 8'`
  (run `cmake build/switch-gl-mc2` first when generated files were added).
- Next toward 30 fps of game pictures / 60: the three threads are now balanced (~14–17 ms each); see the per-thread lines.

### More VU1 coverage (2026-11-16): 149 compiled programs (63 recordings)
- Automated PC runs from save states (scratchpad `variant.sh`): boot → memory card "play without saving" → main menu, then
  Options → Cheat Codes → type **`pennyThug`** on the on-screen keyboard (unlocks Paris/Tokyo in Arcade) → Arcade city row
  (`arcade.state`, frame 4290). Variants: Paris, Tokyo, 3 vehicles, 3 race types, each driving ~1,700 frames → 32 new programs.
- Not covered: split-screen (the game never addresses pad port 2 in our SIO2 emulation — a `PS2_PAD2` plug-in from boot did not
  help, reverted), career beyond the first race, replays. `record_vu1.cmd` (in `mc2/`) records while a person plays on the PC
  (into `games/mc2/vu1_new`; copy into `games/mc2/vu1`, rerun `vurecomp`, rebuild).
- Switch, Tokyo arcade race from the PC save state (`--load-state /mc2/arcade.state` in `args.txt` works on the Switch):
  **0 interpreted starts**, 22–28 ms per frame; VU / GS / emulation threads each only 52–64 % busy → the rest is the threads
  waiting on each other (queue hand-offs, `post_present` keeps ≤ 2 pictures in flight), not one thread's work.
- `PS2_DK_SCREEN_DUMP` counts pictures since start-up, not emulated frames (after a state load it starts at 0).
- PC runner rebuild: `%USERPROFILE%/AppData/Local/Programs/Python/Python313/Lib/site-packages/cmake/data/bin/cmake.exe --build
  build/native-mc2` (with that Python's `Scripts` on PATH for ninja); devkitPro's cmake cannot use that cache.

## GPU triangle renderer (in progress, 2026-11-15)

Spec `docs/superpowers/specs/2026-11-15-switch-hw-renderer-design.md`, plan `docs/superpowers/plans/2026-11-15-switch-hw-renderer-stage0-1.md`,
ledger `...stage0-1.progress.md`. Stage 0 done (`PS2_HWR_STATS=1`, `runtime/src/gs/gpu/hwr/{classify.hpp,stats.cpp}`): race per frame
8-13k shapes; painter-only shapes 3.7-5.5 % of covered pixels (partial frame mask); ~10-11k textures uploaded by the EE,
~600-670 render-to-texture reads; ~100 upload pages/frame land on GS-drawn pages (VRAM scratch reuse: invalidate only);
no downloads/local copies. Gate passed. Menus: partial mask 53 % of pixels (needs FBMSK as a hardware write mask).

**Stage 1 + most of stage 2 done in Ryujinx (2026-11-15, user away):** `PS2_HWR=1` (deko3d build) draws every GS triangle/sprite
with deko3d (`runtime/src/gs/gpu/hwr/{renderer.cpp,state.hpp,vertices.hpp,classify.hpp}`, `dk/hwr.{vert,frag}`, `dk/tex_blit.frag`,
backend image/draw API in `backend_deko3d.cpp`). Textures via the painter's own texel fetch (`glsl_raster_lean.py --texfetch`,
pipeline kTexFetch), render-to-texture from GPU targets, dual-source blending for all race blend formulas, D32F depth, alpha
test, fog, TFX/TCC, whole-byte FBMSK as colour write mask. Race start (picture 3600) vs PC: 6.8 % pixels differ (camera timing),
same brightness. PC tests: `tests/hwr_{classify,vertices,state}_test.cpp`. Self-test: `PS2_HWR_SELFTEST=1|2`. Debug:
`PS2_HWR_LOG=1`, `PS2_HWR_DEBUG=nodepth,noatest,noblend,skipblend<n>`, `PS2_DK_SCREEN_DUMP=/mc2/shot_%u.ppm@300,600,...`.
Gotchas: no `return` in deko3d shader main (PRET → Ryujinx outputs 0); targets are stored bottom row first (flip v when sampled,
region rows in present); new targets must start with alpha 0. **Next: hardware timing run** (`PS2_HWR=1`), in-race frames
(car/HUD) check, stage 3 (painter fallback/write-back, loading targets from GS memory). Plans: `...stage0-1.md`, `...stage2-3.md`.

## 30 fps milestone: overclock + raster (2026-11-15) — current state, start here

Spec `docs/superpowers/specs/2026-11-15-switch-30fps-design.md`, plan `docs/superpowers/plans/2026-11-15-switch-30fps.md`,
ledger (all decisions) `docs/superpowers/plans/2026-11-15-switch-30fps.progress.md`.

- **Clocks from the NRO** (`host/switch_clocks.{hpp,cpp}`, rules in `host/switch_clocks_logic.hpp`, PC test
  `tests/switch_clocks_logic_test.cpp`): requests kit rates CPU 2091 / GPU 1228.8 MHz, reads back, falls back to stock
  1785 / 921.6; GPU raised docked **and handheld** (user's choice); re-set every ~1 s from `poll()`; restored at exit and in
  the crash handler. `PS2_CLOCKS=0` / `PS2_CLOCKS=<cpu>,<gpu>` (MHz). User's console: 2017 model, no kit → stock maximums.
  Ryujinx: logs `[clocks] unavailable`, runs normally.
- **8×8 raster tiles** (`scripts/glsl_raster_lean.py` `TILE8 = True` + `gpu.cpp` `g_tile = 8` on the GL path; must match).
  PC 75 images 0 diff. Ryujinx draws red lines/dots in the race sky with 8×8 + shared setup (not on hardware: user checked).
- `PS2_RASTER_STATS=1` (gpu.cpp): per 300 frames, shapes, thread visits and lane use for 16×16 vs 8×8 tiles. Race: ~7300
  shapes/frame, ~8M bounding-box pixels/frame (~28× the screen), lane use 82 % / 88 % → the cost is per-pixel shading load
  (overdraw), not threads walking past shapes.

**Every hardware number before this section was handheld (GPU 307 MHz).** Hardware, handheld, CPU 1785 / GPU 921 MHz:

| Section | 1020 / 307 MHz, 16×16 | 1785 / 921, 16×16 | 1785 / 921, 8×8 (now) | target |
|---|---|---|---|---|
| Films (frames 0–1100) | ~58 ms | 34–36 ms | 33–35 ms (CPU-bound) | 33.3 ms |
| Menus (1100–2900) | 100–127 ms | 35–45 ms | 34–42 ms (GPU) | 33.3 ms |
| Race (3000–3700) | ~245 ms | 87–92 ms | **75–78 ms (~13 fps)** (GPU; CPU busy ~15 ms) | 33.3 ms |

Not reached: race needs ~2.3× more, menus ~1.2×, films ~1.05×. Docked not measured (user plays handheld; same clocks
apply). Next options: (1) design approach 2 — draw with the GPU's triangle hardware (upstream's experimental `hwdraw` idea
ported to deko3d; v0.0.4 base has none); (2) cheaper per-pixel shading in `plot()` (memory traffic per pixel: framebuffer
read/write + texture fetch emulation); (3) an overclock kit (GPU 1228.8 → ~1.33× more) — the NRO already uses it if present.

## deko3d backend (Phase 3, 2026-11-15) — works on hardware; race is GPU-bound in the raster shader

- Build: `-DPS2_GPU_DEKO3D=ON` in `build/switch-gl-mc2` (currently ON there; the old GL NRO is saved as `build/ps2run_gl.nro`;
  turn the option OFF to build GL again). Only the runtime + `host/window.cpp` recompile (`PS2_GPU_DK` is private), not the generated code.
- Files: `runtime/src/gs/gpu/backend_deko3d.cpp` (same `backend.hpp`), `scripts/glsl_dk.py` (glsl/* buffer images → SSBOs; keeps the
  Mesa hang fix), `runtime/src/gs/gpu/dk/overlay.{vert,frag}`, `cmake/embed_bin.cmake`, CMake option `PS2_GPU_DEKO3D`;
  `host/window.cpp` creates no SDL window under `PS2_GPU_DK` (SDL's Switch window always takes the screen for EGL).
  `host/main.cpp` crash line now prints `handler +0x...` (pc minus `__libnx_exception_handler`; add its `nm` address for the ELF offset).
- Model: CPU writes → 64 MB staging ring + `dkCmdBufCopyBuffer` in command order (no CPU waits); barrier after each dispatch;
  reads wait on a cache-flushing fence. 4 command segments × 4 MB.
- Switches: `PS2_DK_STATS=1` (per 5 s: presents, submits, dispatches, copies, waits, CPU blocked), `PS2_DK_TIMING=1` (GPU time per
  shader id: 1 upload, 4 load_clut, 5 display, 6 raster, 8 snapshot, 9 decode, 10 raster_mip; waits per submit).
- Ryujinx: frames 300/600/3600 0.0 % vs `baseline/pc-gl` (1800 differs only by the unformatted-memory-card dialog).
- Hardware: 900-frame and 3700-frame runs clean. Films ~58 ms/frame (GL ~75), CPU-bound (emulation thread busy ≈ wall).
  Race ~400 ms/frame (~2.5 fps), emulation thread busy ~25 ms; GPU fully busy: per 5 s raster 1470 dispatches 2554 ms
  (1.7 ms each), raster_mip 840 dispatches 1945 ms (2.3 ms each); everything else ~400 ms. → **Next: make the raster shaders
  cheaper** (code size 55 KB; check registers/spills, split texture sampling out of `plot()`, fewer/larger dispatches).
- **Raster registers (done)**: uam's raster used 255 GPRs (dksh program header word at byte 32) → 1 group of 256 threads per SM.
  `scripts/glsl_raster_lean.py` (run by CMake on `gs_raster*.comp` for GL and deko3d): Setup in shared memory, bilinear taps
  through one read site in a non-unrollable loop, `rt_copy` branch dropped at SCALE 1 → raster 118 / raster_mip 122 GPRs, code
  54 → 28 KB. PC GL: all 75 harness images 0 diff. Hardware race: 403 → 245 ms/frame (~4 fps), raster 1.74 → 0.87 ms/dispatch,
  raster_mip 2.3 → 1.36 ms; menus 164–210 → 100–128 ms. Still GPU-bound (~4.65 s GPU per 5 s).
  Next ideas: below 80 GPRs (3 groups/SM) — the `sample_` nearest path and mip `level_setup` still inline extra reads (stubbing
  `sample_` gives 53); fewer barriers between independent raster dispatches; check `PS2_DK_TIMING` overhead is not skewing.
- Round 2: `sample_` rewritten (one read site + one conversion for nearest and bilinear) → raster 111 / raster_mip 115 GPRs,
  PC 75 images 0 diff, but **no speed change** on hardware (still 245 ms race; no occupancy step crossed). Merging `convert`'s
  palette reads gave nothing (reverted). Stubbing `convert` would give 75 GPRs.
- `PS2_DK_BARRIER_TEST=1` (barriers off every other 5 s window): race 20–24 presents/5 s either way → the per-dispatch
  barriers cost nothing measurable. The GPU time is the raster shaders' own work. Likely cause: each of a tile's 256 threads
  walks the tile's whole primitive list (load prim, setup, bbox/edge tests) even where the primitive misses its pixel;
  ~200 ms GPU per race frame is thousands of cycles per shaded pixel. Next real gain = rasterizer structure (e.g. per-tile
  primitive culling/binning finer than 16×16, or skipping threads early), not registers.
- One startup crash (before the game, `EE pc 0`) seen once, not reproduced in 4 later runs.
- Not yet checked by eye on hardware: picture orientation and controller input without an SDL window.

## Resolved: `decode` GPU hang on real Switch (2026-11-15)

- **Root cause**: Hardware bisection (`PS2_GL_DIAG` + non-DCE shader cuts) proved Mesa `20.1.0-rc3` (`NV120`) hung whenever `gs_decode.comp` passed the 152-byte `Setup` struct by value into `convert(Setup, uint)` and `expand16_(uint, Setup)` with early `return`s in `sw0_` branches (cut0..cut4 all passed in `0.39–1.04 ms`; cut5 `convert` hung).
- **Fix applied**:
  - Scalarized `expand16_(uint c, uint flags, uint ta0, uint ta1)` and `convert(uint tpsm, uint flags, uint ta0, uint ta1, uint clut, uint cpsm, uint csa, uint raw)` with a single `return res;` in:
    - `MC2Recomp/ps2-native-recomp/runtime/src/gs/gpu/glsl/gs_decode.comp`
    - `MC2Recomp/ps2-native-recomp/runtime/src/gs/gpu/glsl/gs_raster.comp`
    - `MC2Recomp/ps2-native-recomp/runtime/src/gs/gpu/glsl/gs_raster_mip.comp`
    - Note: `scripts/glsl_compat.py` was not updated yet; if re-running `scripts/shaders.sh` from `.wgsl`, port this scalar `expand16_`/`convert` pass or keep the committed `.comp` files.
  - Cleaned all temporary diagnostic code from `MC2Recomp/ps2-native-recomp/runtime/src/gs/gpu/backend_gl.cpp`.
- **Fresh-buffer safety on Switch**:
  - `write_buffer_fresh` in `backend_gl.cpp` defaults to `fresh_mode=2` (`gpu_idle() + cpu_write`) under `#ifdef __SWITCH__` (PC keeps `0` = `glBufferData(nullptr)`), overridable via `PS2_GL_FRESH_MODE`.
  - Verified on hardware that `PS2_GL_FRESH_MODE=1` (`glBufferData(nullptr) + make_texture_view` without `gpu_idle()`) triggers an ARM64 data abort (`exception 0x101`, `far 0x18`) at `ROCKSTAR.PSS`. Do not switch Switch default below `2` on Mesa GL.

## Hardware benchmark & validation results (Switch GL, 2026-11-15)

- **Run 2 (`--frames 900`, default `fresh_mode=2`, `PS2_IOP_TIME=1`, no PPM dumps)**: `900/900` frames, 0 hangs (`1,463` submits, `2,638` uploads, `879` displays, `874` rasters, `436` decodes). FMV ran `13.3–17.7 fps` (~`75.0 ms/frame` wall; `program 12` `raster 896x1x1` x2 = `~68 ms/frame`, `decode 1024x16x1` = `~5 ms`; IOP `5.5–5.7 ms/frame`).
- **Run 3 (`--frames 4500`, `harness.sh` button inputs, `--frames-dir /mc2/frames` every 60th frame)**: `4,500/4,500` frames completed cleanly (`exit 0`, 0 hangs): `5,140` submits, `108,998` uploads, `91,291` `load_clut`, `4,556` displays, `233,396` rasters, `136,350` snapshots, `22,169` decodes, `463,048` mipmap setups, `13,476,325` primitives drawn.
  - Scene frame wall times during Run 3 (includes SD PPM write overhead): logos `130–180 ms/frame`, menu `276–338 ms/frame`, `MOSES_01.PSS` `127–180 ms/frame`, in-game race `717–1033 ms/frame` (~`1.0–1.3 fps`).
  - Pixel comparison vs `baseline/pc-gl`: `frame_00300.ppm` (`01_logo_rockstar`) = `0/228864 (0.000%)`, `frame_00600.ppm` (`02_logo_sandiego`) = `0/228864 (0.000%)`, `frame_04500.ppm` (`05_race_ingame`) = `429/228864 (0.187%)`; `frame_01800.ppm` shows crisp memcard format UI (differs from `03_menu.png` only because Switch had an unformatted `/mc2/mc2.ps2` present).

## Real-Switch GPU findings (Mesa 20.1 nouveau on GM20B) — handled in `runtime/src/gs/gpu/backend_gl.cpp`

Found with `PS2_GL_SELFTEST=1` (or `=exit` to stop after the tests). Each is handled behind `__SWITCH__` where it costs time:
- **glFinish / fence wait return one step early**: every CPU read after one glFinish was wrong; after two, none. → `gpu_idle()` (two glFinish on Switch) used for all waits and `write_buffer_fresh` (`fresh_mode=2`).
- **Buffer copies (`glCopyBufferSubData`, and glBufferSubData into a busy buffer via Mesa's staging copy) land late.** → copies are a compute shader (`copy_program()`), partial uploads wait for idle first.
- **CPU cache**: transfers go through `glMapBufferRange` + libnx `armDCacheFlush` before/after (`cpu_write`, `cpu_read`).
- Self-test still reports "atomics + offset uniforms" FAILED on hardware: that test reads with a single raw glFinish (test artifact, not a backend bug). Everything else passes.

## Ryujinx findings (see `to-do.md` "Ryujinx findings" for details)

- C++ throw crashed Ryujinx (GCS instructions in GCC 16 libgcc) → `scripts/nx_nop_chkfeat.py` patches every Switch link.
- Ryujinx cannot run Mesa's LD/ST (storage buffers), PRET, JCAL, `switch` → `scripts/glsl_compat.py` converts naga's GLSL (images instead of SSBOs, wrapped main, udiv_/umod_, if-chains), also removes naga's WebGPU y flip (picture was upside down).
- Ryujinx must use its own config: `%APPDATA%\Ryujinx\Config.json` is the user's (Vulkan, HostMappedUnsafe). Keep untouched.

## IOP ahead-of-time translation (done, verified)

- Nearly all IOP time was one AGESND thread polling the clock. `PS2_IOP_HIST=<file>` (capture) → `scripts/iop_recomp.py <file> <out.cpp>` → CMake `-DPS2_IOP_REGIONS=<cpp>`. MC2: `games/mc2/iop/iop_regions.cpp` (139 regions, 99.5% of executed IOP instructions). Logs/pixels/cycle count identical to the interpreter.
- IOP time: PC `3.9 → 0.6 ms/frame`, Switch hardware `5.4–6.1 ms/frame`, Ryujinx `24 → ~8 ms/frame`. Switches: `PS2_IOP_INTERPRET=1`, `PS2_IOP_REGIONS=a-b`, `PS2_IOP_REGION_TRACE=1`, `PS2_IOP_TIME=1`.

## Files changed (toolkit: `MC2Recomp/ps2-native-recomp`)

- `runtime/src/gs/gpu/backend_gl.cpp` — image-buffer bindings, `fresh_mode` (`2` default on Switch, `0` on PC), Switch sync/cache workarounds, compute copy, self-test, debug switches (`PS2_GL_TIMING=1|2`, `PS2_GL_SKIP`, `PS2_GL_TRACE`, `PS2_GL_SCREEN_DUMP=<file>@n`, `PS2_GL_FRESH_MODE=0|1|2`).
- `runtime/src/gs/gpu/backend.hpp`, `gpu.cpp` — `write_buffer_fresh`, `PS2_VRAM_DUMP=<file>@n`, 1× scale / no depth-frac on GL.
- `runtime/src/gs/gpu/glsl/*` — regenerated + converted (`gs_decode.comp`, `gs_raster.comp`, `gs_raster_mip.comp` scalarized `expand16_`/`convert` for Mesa `20.1.0-rc3`); `scripts/shaders.sh` runs naga then `scripts/glsl_compat.py`.
- `runtime/src/iop/cpu.cpp`, `iop.hpp` — regions, histogram capture, IOP timing.
- `host/main.cpp` — Switch exception handler (`sdmc:/mc2/crash.txt`), nxlink stderr, `sdmc:/mc2/env.txt` (`NAME=value`) and `sdmc:/mc2/args.txt` (extra arguments).
- `host/window.cpp` — controller-only input on Switch, lazy `PS2_FRAME_LOG`, accelerated SDL renderer, GL present message.
- `runtime/src/disc.cpp` — sequential-only read-ahead with a lock. `runtime/src/ipu/ipu.cpp` — reverted to the original.
- `CMakeLists.txt` — `PS2_IOP_REGIONS`, chkfeat post-link step. `scripts/port.sh --switch` (uses `PS2_GENERATED`, GL by default).
- New: `scripts/glsl_compat.py`, `scripts/iop_recomp.py`, `scripts/nx_nop_chkfeat.py`, `games/mc2/iop/*`.
- Project root: `tools/ryujinx_run.sh`, `tools/ryujinx_compare.py`, `tools/harness.sh`, `tools/compare.py`, `baseline/*`.

## How to build and test

```bash
# Switch NRO (devkitPro msys shell; plain Git Bash cmake/make fail on TMP)
MSYSTEM=MSYS /c/devkitPro/msys2/usr/bin/bash -lc "cd $HOME/Downloads/mc2/MC2Recomp/ps2-native-recomp && export TMP=/tmp TEMP=/tmp && make -C build/switch-gl-mc2 ps2run_nro -j16"
```

```bash
# PC OpenGL, optimized (llvm-mingw + Python's cmake.exe/ninja on PATH)
export PATH="$HOME/AppData/Local/Microsoft/WinGet/Packages/MartinStorsjo.LLVM-MinGW.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/llvm-mingw-20261020-ucrt-x86_64/bin:$HOME/AppData/Local/Programs/Python/Python313/Scripts:$PATH"; ninja -C build/gl-mc2-o3 ps2run
```

```bash
# PC pixel check against baseline/pc-gl (Windows Python 3.13 has Pillow)
cd $HOME/Downloads/mc2 && PS2_RUN=$PWD/MC2Recomp/ps2-native-recomp/build/gl-mc2-o3/ps2run.exe bash tools/harness.sh /tmp/out && $HOME/AppData/Local/Programs/Python/Python313/python.exe tools/compare.py baseline/pc-gl /tmp/out --tol 0 --max-pct 0
```

Real Switch (IP <switch-ip>):
- SD layout `sdmc:/mc2/`: `mc2.iso` (3.2 GB), `BOOT.ELF`, `patches.txt`, `toggles.txt`, `env.txt` (currently cleared/empty), `args.txt` (currently cleared/empty), `quick/`, `frames/` (PPM captures from Run 3).
- FTP server on port 5000 (anonymous): `curl -T file ftp://<switch-ip>:5000/mc2/file`, `curl -o f ftp://<switch-ip>:5000/mc2/switch_log.txt`.
- nxlink: `nxlink.exe -a <switch-ip> -s build/switch-gl-mc2/ps2run.nro` (`-s` streams stdout/stderr back; do NOT pass `-p`). Netloader must be open in hbmenu (`Y`) launched via title override (hold `R` on a game); press `Y` before each push. Never port-scan `28280`.

## Suggested next steps

1. **Phase 3 — Native `deko3d` backend (`backend_deko3d.cpp` on `backend.hpp`)**: Switch GL compute is cleanly verified for correctness (`0.000%`–`0.187%` diff across 4,500 frames) and decisively GPU-bound (~`13.3 fps` FMV, ~`1.0–1.3 fps` race) under Mesa `20.1.0-rc3` (`gpu_idle()` + per-dispatch overhead). Implement `backend_deko3d.cpp` (`uam`-compiled compute/graphics shaders, `DkMemBlock` buffers, `dkCmdBufDispatchCompute`, barriers without `glFinish` double-waits, `dkSwapchain` present).
2. **(Optional before/alongside deko3d, `to-do.md` N.3)**: Profile or split texture sampling out of `plot()` in `gs_raster.comp` (`usamplerBuffer` / smaller register pressure) if keeping GL as a fallback, or port that improvement directly into the `deko3d` shader pipeline.
3. **Sync `scripts/glsl_compat.py`**: Add scalarization of `expand16_`/`convert` if `scripts/shaders.sh` will be re-run from `gs.wgsl`.
4. **Optional convenience asked by the user**: "wait for the next build" mode in the NRO (`nxlink` protocol on port `28280` + `envSetNextLoad`) on exit/crash so netloader needs no `Y` press between pushes.
