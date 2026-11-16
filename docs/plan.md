# Midnight Club II → Nintendo Switch homebrew port: plan

## Context

Goal: run Midnight Club II (PS2, SLUS-20209) as a Switch 1 homebrew `.nro`, built from the existing
recomp stack in `%USERPROFILE%\Downloads\mc2`:

- `MC2Recomp/` — game-specific project (launcher `hub/`, option patches `mc2-options/`, and
  `patches/ps2-native-recomp.patch`, 23 files of toolkit fixes MC2 needs). Pins toolkit **v0.0.4** as submodule.
- `ps2-native-recomp/` (0.0.5 also present) — the toolkit: `recomp/` (EE/VU → C++ translator, runs on PC),
  `runtime/` (emulated GS/VU/IOP/DMAC/SPU2 etc.), `host/` (SDL2 window, input, main).
- `Midnight Club II (USA).iso` (3.2 GB), `MC2Recomp-0.5/` prebuilt Windows release (ps2run.exe 58 MB).
- Test targets: Ryujinx 1.3.3 (keys + firmware installed) and a real Switch with Atmosphère.
- Done = boots and races are playable in Ryujinx; then performance on real hardware (user: "performance is
  important in the end").

## What blocks a Switch build today (from the survey)

| Blocker | Where | Switch reality | Fix |
|---|---|---|---|
| Live translation: unseen EE/VU code is compiled with clang++ at play time, loaded via `LoadLibrary`/`dlopen` | `runtime/src/live_build.cpp`, `ee_live.cpp`, `vu_live.cpp` | no compiler, no dlopen | AOT-only build: gather full coverage on PC first; stub live path; unseen code → crash report (EE) / interpreter (VU, already exists: `vu_micro.cpp`) |
| GS drawn by WebGPU compute shaders (wgpu-native, `gs.wgsl` 1619 lines, `gpu.cpp` ~3500) | `runtime/src/gs/gpu/` | no wgpu; devkitPro Mesa gives OpenGL 4.x/GLES 3.2 + EGL; no Vulkan ICD on HOS homebrew; deko3d is native | Phase 1: CPU rasterizer (`raster.cpp`, selected when `PS2_GPU` undefined). Phase 2: OpenGL backend. Phase 3: deko3d if GL too slow |
| x86 intrinsics | `vu_ops.hpp` (31, has `#else` scalar paths), `hwdraw.cpp` (39), `ipu.cpp` (27), `vif.cpp` (20) | ARM64 Cortex-A57 | `sse2neon.h` header drop-in (keeps SSE source), scalar `#else` for the rest |
| Win32/Linux host bits | `host/window.cpp` (SDL2 + Win32 fg-window), `crash.cpp` (1109 lines, dbghelp/signals), `profile_win.cpp`, `web.cpp` | libnx has SDL2 (portlibs), no dbghelp | SDL2 stays; `#ifdef __SWITCH__` guards; drop crash/profile/web |
| Threads: ~10 `std::thread` sites (GS queue, VU worker, IOP, audio) | `system.cpp`, `gs/gpu/*`, `host/*` | 3 cores for homebrew, threads need explicit stack sizes & core affinity | pthread via libnx works; pin GS/VU/EE to cores 0–2; size stacks (`big_stack_thread.hpp` exists) |
| Toolchain: llvm-mingw / clang | `CMakeLists.txt`, `scripts/port.sh` | devkitA64 is GCC (aarch64-none-elf), CMake toolchain at `C:\devkitPro\cmake\Switch.cmake` | new CMake toolchain path; fix clang-only constructs as they appear |
| Disc I/O: 3.2 GB ISO via `fseeko/ftello` | `disc.cpp` | ISO on SD (`sdmc:/`), FAT32 4 GB limit OK (3.2 GB) or exFAT; Ryujinx virtual SD at `%APPDATA%\Ryujinx\sdcard` | path change only; buffer reads (SD is slow) |
| Memory: `g_ram` 32 MB static + GS 4 MB + buffers; NRO itself ~100 MB | `memory.cpp` | applet mode ≈ 0.5 GB heap; **title-override mode** ≈ 3+ GB | always launch via title override (hold R on a game); same in Ryujinx |

devkitPro is already installed at `C:\devkitPro` (devkitA64, libnx, portlibs: SDL2, EGL, GLES, mesa/nouveau, glad).

## Approaches considered (GS backend)

1. **CPU rasterizer → OpenGL compute → deko3d** (chosen). CPU path exists behind a flag → boots earliest; GL
   reuses `gs.wgsl` via naga WGSL→GLSL; deko3d only if GL (Mesa nouveau overhead) can't reach speed.
2. Straight to OpenGL: nothing on screen until the whole wgpu→GL port works; harder to separate GS bugs from
   CPU-side port bugs.
3. deko3d first: best perf ceiling, biggest rewrite, poor Ryujinx debuggability.
4. Vulkan: user asked for it as step 2. **Not viable**: devkitPro Mesa ships no Vulkan driver for HOS; nothing
   to link against. Ladder is therefore GL → deko3d. (Revisit only if a libnx Vulkan ICD appears.)

## Plan

### Phase 0 — Baseline on PC and full code coverage (no Switch code yet)

Why first: the Switch build must contain *all* game code ahead of time. The toolkit already records every
entry point / rewritten code window the game reaches (`games/mc2/entries.txt`, `overlays/`, `misses.txt`),
and `port.sh` has an auto-explore "round" loop.

1. Build MC2Recomp on PC as documented (`tools/build.sh "<iso>"`) → confirms toolchain, patch, generated
   code. **Toolkit decision (0.2):** stay on MC2Recomp's pinned, patched toolkit (v0.0.4 `830c7c33` +
   `patches/ps2-native-recomp.patch`); verified via `git apply --check` that the patch does not apply cleanly
   to `ps2-native-recomp-0.0.5` or `v0.0.5-31-g5d1ebbc` (conflicts in `keymap.*`, `window.cpp`, `gpu.cpp`,
   `gs.wgsl`, `gs.cpp`, `cdvdfsv.cpp`, `spu2.cpp`, `system.cpp`, `vu_micro.cpp`, etc.).
2. Play through broadly (every city, every mode, split-screen, menus, options, movies, pause, save/load)
   with live translation on. Each new reach appends to `entries.txt`/overlays. Re-run `port.sh` rounds until
   "0 new entry points, 0 new window images" after a long session.
3. Add a runtime option to **refuse live translation** (`PS2_LIVE=0`): missing code → logged
   `misses.txt` + clean stop instead of compiling. Verify a full PC play session with it off produces no
   misses. This is the gate for Phase 1; it is also what the Switch runtime will do.
4. Set a regression harness now, reused on Switch: headless `ps2run --frames N --frames-dir ... --press ...`
   deterministic runs → screenshots of intro, menu, first race start. Compare PC vs Ryujinx later.

### Phase 1 — Switch bring-up: CPU rasterizer, boots in Ryujinx

Work in a new `switch/` folder in the toolkit fork + a `switch` CMake preset; keep diffs small and
`#ifdef __SWITCH__`-guarded so upstream merges stay possible.

1. **Toolchain**: `cmake -DCMAKE_TOOLCHAIN_FILE=C:/devkitPro/cmake/Switch.cmake`, `nx_create_nro()` for the
   output. Compile `recomp/` tools on PC only (they are host tools); only `runtime/` + `host/` + generated
   code cross-compile. Expect GCC-vs-clang fixes (`__builtin_*`, attributes, `<format>`), AVX flags removed
   (`-march=armv8-a+crc+crypto -mtune=cortex-a57`).
2. **Intrinsics**: add `third_party/sse2neon.h`; in the four intrinsic files `#if defined(__aarch64__)
   #include "sse2neon.h"`. `vu_ops.hpp` already has scalar `#else` branches — compare both for speed later
   (VU math is hot).
3. **Live translation off**: compile `live_build.cpp` / `ee_live.cpp` / `vu_live.cpp` out on Switch
   (`PS2_NO_LIVE`); `dispatch.cpp` line ~267 path reports "missing code" and halts with the address.
   VU programs not in the build run in `vu_micro.cpp` interpreter (already the fallback when no compiler).
4. **Host**: `host/main.cpp` gets a Switch entry: hardcoded args equivalent to
   `ps2run BOOT.ELF --disc sdmc:/mc2/mc2.iso --game-dir sdmc:/mc2 --memcard sdmc:/mc2/mc2.ps2 --fullscreen`
   + `--patches` from `mc2-options/toggles.txt` (widescreen default on for 16:9 screen). `romfs` for small
   assets, SD for ISO/saves. `window.cpp`: SDL2 on libnx (1280×720 docked/handheld), SDL game controller =
   Joy-Con/Pro; remove Win32 `ShowWindow` block. Compile out `crash.cpp` dbghelp/signal parts,
   `profile_win.cpp`, `web.cpp`, `quickmenu.cpp` keyboard paths. Audio: SDL2 audio works on libnx.
5. **Threads/memory**: `svcSetThreadCoreMask` pins: EE main core 0, GS core 1, VU/IOP/audio core 2.
   Stack sizes explicit (libnx default 128 KB is too small for recompiled EE code → use
   `big_stack_thread.hpp` sizes). Confirm NRO runs in title-override mode; log heap at start.
6. **Graphics**: build with `PS2_GPU` undefined → `gs/raster.cpp` CPU path; present the 640×448 buffer via
   SDL2 texture (or EGL quad). Slow but correct.
7. **Test in Ryujinx**: copy ISO into `%APPDATA%\Ryujinx\sdcard\mc2\`; load `.nro` via Ryujinx "Load
   Application from File"; enable Ryujinx logging for guest `svc` errors. Milestones: Sony/Rockstar logos →
   intro movie → main menu → one arcade race → save/load.
8. **Real hardware**: deploy with `nxlink -s` (stdout back over network) for crash output; same milestones;
   record fps (`PS2_FPS` overlay or stderr counter).

### Phase 2 — OpenGL GS backend (performance)

The GS GPU path is ~3500 lines of `gpu.cpp` wgpu calls + `hwdraw.cpp` (card-friendly draws as render
pipelines) + `gs.wgsl`/`blit.wgsl` compute. Mesa on Switch: OpenGL 4.x core incl. compute shaders and SSBOs.

1. Translate shaders offline: `naga gs.wgsl gs.comp` (GLSL 430 compute, one `.comp` per entry point via
   naga's `--entry-point`); the `kEntryPoints[]`/override-constant table in `gpu.cpp` ~3450 becomes GL
   program variants. Commit generated GLSL; keep WGSL as source of truth.
2. Thin abstraction over the wgpu calls `gpu.cpp` actually uses (buffer create/write/map, bind group =
   SSBO binding indices, compute pass dispatch, render pipeline for `hwdraw`, present). Implement it on
   `glad`+EGL (libnx) — not a general RHI; just the ~15 wgpu functions used. Features: `DualSourceBlending`
   → `GL_ARB_blend_func_extended`; `TimestampQuery` → `GL_ARB_timer_query`/off.
3. First validate the GL backend **on PC** (Mesa/NVIDIA GL on Windows) against the wgpu build with the Phase 0
   screenshot harness — pixel-compare. Only then build it for Switch (Ryujinx's GL emulation is slow/odd;
   real hardware is the truth here).
4. Measure on hardware per scene (menu, race, split-screen): CPU frame time vs GPU frame time. Tune: run
   at 1× PS2 resolution, overclock via sys-clk for a data point, `hw::enabled()` card path on.

### Phase 3 — deko3d (only if Phase 2 misses target)

Same thin abstraction, second implementation on deko3d: shaders via `uam` (GLSL → deko3d), SSBOs,
compute dispatch, swapchain present. Skips Mesa's validation/overhead; typically 1.5–3× faster than nouveau
GL on Switch. Not debuggable in Ryujinx beyond "does it render". Decide after Phase 2 numbers.

### Phase 4 — Polish (after playable)

MC2 option patches as a simple in-game/startup menu (60 FPS patch likely off: CPU-bound), 2-player with two
controllers, save on SD, NRO icon/nacp, `README-switch.md` build + run instructions, release zip without
game data (same legal stance as MC2Recomp).

## Key files to touch

- `ps2-native-recomp/CMakeLists.txt` (+ new `cmake/switch.cmake`), `scripts/port.sh` (`--switch` target)
- `runtime/src/live_build.cpp`, `ee_live.cpp`, `vu_live.cpp`, `dispatch.cpp` — compile-out / `PS2_LIVE=0`
- `runtime/include/ps2/vu_ops.hpp`, `runtime/src/gs/gpu/hwdraw.cpp`, `runtime/src/ipu/ipu.cpp`,
  `runtime/src/vif.cpp` — sse2neon / scalar
- `host/main.cpp`, `host/window.cpp`, `host/crash.cpp`, `host/big_stack_thread.hpp` — Switch host
- `runtime/src/gs/gpu/gpu.cpp`, `hwdraw.cpp`, `gs.wgsl`, `blit.wgsl` — Phase 2 GL backend
- `MC2Recomp/patches/ps2-native-recomp.patch`, `mc2-options/` — carried over unchanged (apply before build)

## Risks (honest)

- **Performance** is the big one: 3× Cortex-A57 @1.02 GHz vs the PC build that is *already slower than
  PCSX2* on a laptop. Expect well under full speed on CPU raster; GL/deko3d GS and VU NEON tuning decide
  whether 30 fps is reachable. Ryujinx says nothing about this.
- **Coverage gaps**: any code path not exercised on PC halts on Switch. Mitigation: Phase 0 step 3 gate +
  `misses.txt` round trip (play on Switch, copy misses back, rebuild).
- **Mesa nouveau on HOS** is a hobby driver: compute-shader SSBO bugs possible → deko3d fallback planned.
- **GCC vs clang** differences in a 22k-line C++20 codebase + MBs of generated code; compile times for
  generated code on aarch64-g++ (tens of minutes).

## Verification

- Phase 0: `PS2_LIVE=0` full PC session → `misses.txt` empty; screenshot harness baseline images.
- Phase 1: NRO boots in Ryujinx to menu and completes a race; same on hardware via nxlink; screenshots match
  PC baseline (CPU raster is the reference rasterizer).
- Phase 2: GL backend on PC pixel-matches wgpu on the harness frames; on hardware fps logged per scene,
  target ≥30 fps in races at 1×.
- Each phase: `python scripts/autotests.py` still 54/55 on PC (no regressions upstream-side).
