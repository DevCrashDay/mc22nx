# Midnight Club II → Switch homebrew: technical to-do

**Current state and next steps: see `HANDOFF.md` (2026-11-15).**

Companion to `plan.md` (rationale, risks). This file is the executable checklist for a coding agent.
Work top to bottom; each phase has an exit gate. Tick boxes as you go. Keep every Switch change behind
`#ifdef __SWITCH__` / CMake options so the Windows/Linux build keeps working.

## Environment (verified 2026-11-14)

- Repo root: `%USERPROFILE%\Downloads\mc2`
  - `MC2Recomp/` — game project. Pins toolkit v0.0.4 as submodule + `patches/ps2-native-recomp.patch` (23 files).
  - `ps2-native-recomp/` — toolkit (also `ps2-native-recomp-0.0.5/`). Code: `recomp/` (host tools), `runtime/`, `host/`.
  - `Midnight Club II (USA).iso` — 3.2 GB, SLUS-20209.
  - `MC2Recomp-0.5/` — prebuilt Windows release (`game/build/native-mc2/ps2run.exe`, 58 MB).
- Toolchains on this PC: `C:\devkitPro` (devkitA64 = aarch64-none-elf-gcc, libnx, `cmake/Switch.cmake`,
  portlibs: SDL2, EGL, GLESv2, mesa/nouveau, glad, libdrm_nouveau), `C:\devkitPro\msys2` (cmake, python), ninja
  (Python Scripts), Docker. llvm-mingw for the PC build: install via `ps2-native-recomp/scripts/bootstrap.ps1` if missing.
- Ryujinx 1.3.3: `%USERPROFILE%\Downloads\ryujinx-1.3.3-win_x64\publish`, keys + firmware already installed.
  Virtual SD: `%APPDATA%\Ryujinx\sdcard`.
- Real Switch with Atmosphère available (deploy with `nxlink`).
- Shell: Git Bash for `*.sh`; devkitPro msys2 shell for Switch cmake/ninja.

## Phase 0 — PC baseline + full code coverage

Gate: Windows build runs MC2 with live translation disabled and reports zero misses after a full play session.

- [x] 0.1 Build MC2Recomp as documented:
      `cd MC2Recomp && bash tools/build.sh "$HOME/Downloads/mc2/Midnight Club II (USA).iso"`.
      Record: toolkit commit used (`830c7c33`), patch applies cleanly, output `ps2-native-recomp/build/native-mc2/ps2run.exe`.
- [x] 0.2 Decide toolkit base: stay on MC2Recomp's pinned v0.0.4+patch (`patches/ps2-native-recomp.patch`
      fails `git apply --check` on 0.0.5 and `v0.0.5-31-g5d1ebbc`). Documented in `plan.md`.
- [ ] 0.3 Coverage run. Play broadly (all cities, Career + Arcade, split-screen 2P, every menu, options, movies,
      pause, save, load, game over/restart). Live translation appends to `ps2-native-recomp/games/mc2/entries.txt`,
      `overlays/`, `misses.txt`. Then re-run `scripts/port.sh mc2 <iso> --native-only` rounds until it prints
      `0 new entry points, 0 new window images`.
- [x] 0.4 Add runtime switch `PS2_LIVE=0` (env var, read via `ps2::os::env`) in `runtime/src/ee_live.cpp` and
      `runtime/src/vu_live.cpp`: when set, never spawn a compiler. EE miss → append to `misses.txt`
      (`g_options.misses_path`) and stop with the existing `missing code at 0x%08X` reason (`dispatch.cpp` ~L267).
      VU miss → interpreter (`vu_micro.cpp`, already the no-compiler fallback).
- [ ] 0.5 Gate check: full play session with `PS2_LIVE=0` → `misses.txt` empty. If not, feed misses into
      `entries.txt`, rebuild, repeat.
- [x] 0.6 Screenshot harness `tools/harness.sh`: headless deterministic runs
      `ps2run BOOT.ELF --disc <iso> --frames N --frames-dir out/<scene> --frame-at F --press <inputs>` for
      scenes: logos, intro movie frame, main menu, first arcade race frame 1 and frame ~600. Save PNGs as
      baseline `baseline/pc-wgpu/`. Add `tools/compare.py` (PIL, per-pixel diff count + threshold).
- [x] 0.7 Sanity: `python scripts/autotests.py` → 54/55 (54 PASS, 1 DIFF `vu/lower/efu`, 0 HANG, 0 FAIL).

## Phase 1 — Switch bring-up (CPU rasterizer), boots in Ryujinx

Gate: `.nro` boots to main menu and completes one arcade race in Ryujinx and on hardware.

### 1a Build system
- [x] 1.1 `cmake/switch.cmake` (include `$DEVKITPRO/cmake/Switch.cmake`), CMake option `PS2_SWITCH`.
      Only `runtime/`, `host/`, generated code cross-compile; `recomp/` tools and `ps2*test` targets excluded
      when `PS2_SWITCH`. Output via `nx_create_nro(ps2run ...)` with `.nacp` (title "MC2 Switch", author, version)
      and placeholder icon (original art, 256×256 JPG).
- [x] 1.2 Flags: drop `-mavx2`/`-march=x86-64-v3` paths; add `-march=armv8-a+crc+crypto -mtune=cortex-a57
      -O2 -ffunction-sections`. Link: `-lSDL2 -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lnx -lm -lpthread` (GL libs
      only needed from Phase 2; add now so link order is settled).
- [x] 1.3 `scripts/port.sh --switch mc2`: reuses `generated/` from the PC run (same entries), configures with
      `cmake/switch.cmake`, builds `ps2run.nro`. Must not call the live compiler setup.
- [x] 1.4 Fix GCC-vs-clang errors as they appear (`__builtin_*` differences, clang attributes, `<format>`,
      `[[clang::...]]`, `__int128` paths). Keep fixes minimal and portable.

### 1b Portability
- [x] 1.5 `third_party/sse2neon/sse2neon.h` (MIT). In `runtime/include/ps2/vu_ops.hpp`,
      `runtime/src/gs/gpu/hwdraw.cpp`, `runtime/src/ipu/ipu.cpp`, `runtime/src/vif.cpp`: on `__aarch64__`
      include sse2neon instead of `<immintrin.h>` and define `__SSE2__`/`__SSE4_1__` so existing `#if` paths
      compile. `vu_ops.hpp` scalar `#else` branches stay as a fallback; benchmark both later (1.17).
- [x] 1.6 Compile out live translation on Switch: `PS2_NO_LIVE` define → `live_build.cpp`, `ee_live.cpp`,
      `vu_live.cpp` reduce to stubs returning "not available"; `dispatch.cpp` miss path unchanged (writes
      `misses.txt` on SD, halts with address on screen/log).
- [x] 1.7 `host/crash.cpp`: `__SWITCH__` → minimal handler: print reason + PC + EE PC to `stderr` (nxlink) and a
      `sdmc:/mc2/crash.txt`, then `exit`. Exclude dbghelp/libunwind/signal-walk code. Exclude
      `host/profile_win.cpp`, `host/web.cpp`, `host/pad_test.cpp` from the Switch target.
- [x] 1.8 `host/window.cpp`: remove Win32 `ShowWindow`/`SetForegroundWindow` block under `__SWITCH__`;
      window = `SDL_CreateWindow` 1280×720 fullscreen; controllers via `SDL_GameController`
      (libnx SDL2 maps Pro Controller/Joy-Con); keyboard/mouse code paths compiled out; quickmenu (F-keys)
      → map to `+`/`-` buttons or disable. Audio: SDL2 audio as is.
- [x] 1.9 `host/main.cpp` Switch entry: `romfsInit`, `socketInitializeDefault` + `nxlinkStdio` (debug builds),
      then build argv equivalent to
      `ps2run sdmc:/mc2/BOOT.ELF --disc sdmc:/mc2/mc2.iso --game-dir sdmc:/mc2 --memcard sdmc:/mc2/mc2.ps2
       --quick-dir sdmc:/mc2/quick --misses sdmc:/mc2/misses.txt --fullscreen --patches sdmc:/mc2/toggles.txt`.
      `BOOT.ELF` = `SLUS_202.09` extracted by `port.sh` (copy to SD). Widescreen patch default on.
- [x] 1.10 Disc I/O `runtime/src/disc.cpp`: `fseeko/ftello` path already POSIX; add 2 MB read-ahead buffer
      (SD card latency) behind `__SWITCH__`. Verify `_FILE_OFFSET_BITS=64` for >2 GB.
- [x] 1.11 Threads: wrap thread creation sites (`system.cpp`, `gs/queue.cpp`, `gs/gpu/*`, `iop/libs.cpp`,
      `host/main.cpp`, `big_stack_thread.hpp`) with explicit stack sizes (EE main ≥ 8 MB, others ≥ 1 MB) and
      core affinity via `pthread_attr_setaffinity_np`/`svcSetThreadCoreMask`: EE core 0, GS core 1,
      VU+IOP+audio core 2. Count threads at start; libnx limit is fine but log it.
- [x] 1.12 Memory: confirm `g_ram` (32 MB static) + GS VRAM + NRO (~100 MB arm64) fit. Log `svcGetInfo`
      heap size at start; abort with on-screen message if < 1 GB (= applet mode; must use title override).
      Ryujinx: launch NRO as application (not via hbmenu applet).
- [x] 1.13 GS: Switch target compiles **without** `PS2_GPU` → `runtime/src/gs/raster.cpp` CPU path. Present the
      framebuffer each vsync via `SDL_Texture` streaming (`SDL_UpdateTexture` + `SDL_RenderCopy`), letterboxed.

### 1c Test
- [~] 1.14 Ryujinx (GL build; logos, films, ESRB and menu checked; race finish and save/load not reached yet): `mkdir %APPDATA%\Ryujinx\sdcard\mc2`, hardlink/copy ISO as `mc2.iso`, copy `BOOT.ELF`,
      `toggles.txt`. Ryujinx → File → Load Application From File → `ps2run.nro`. Enable Logging → Guest/Error.
      Milestones, each a screenshot into `baseline/switch-ryujinx/`: logos → intro movie → main menu → arcade race
      start → race finish → save → reload save.
- [x] 1.15 Compare Ryujinx screenshots vs PC baseline with `tools/compare.py` (CPU raster vs wgpu: allow dither
      tolerance). Investigate diffs > threshold.
- [ ] 1.16 Hardware: `nxlink -s ps2run.nro` with same SD layout. Same milestones. Log fps per scene
      (add `PS2_FPS=1` stderr counter if none exists). Record in `plan.md` table: scene / fps CPU-raster.
- [ ] 1.17 Micro-benchmark VU: `vu_ops.hpp` sse2neon vs scalar on hardware (`ps2vuregtest` ported or a
      10-second race run). Keep the faster.

## Phase 2 — OpenGL GS backend

Gate: GL backend pixel-matches wgpu on PC harness; on hardware ≥ 30 fps in races at 1× resolution.

- [x] 2.1 Inventory every `wgpu*` call in `runtime/src/gs/gpu/gpu.cpp`, `hwdraw.cpp`, `hwcache.cpp`,
      `host/window.cpp` → list in `docs/switch-gl.md` (expected ~15 distinct functions: instance/adapter/device,
      buffer create/write/map, bind group layout/bind group, compute pipeline + override constants, compute pass
      dispatch, render pipeline (hwdraw), command encoder/submit, surface present, timestamp queries).
- [x] 2.2 Shaders: install `naga-cli` (Rust, `cargo install naga-cli`). Translate `gs.wgsl` and `blit.wgsl`
      per entry point: `naga gs.wgsl gs_<entry>.comp --entry-point <entry>` → GLSL 430 compute. Override
      constants (`cd.compute.constantCount` in gpu.cpp ~L3472) → `#define` prefix injected at load, one GL program
      per variant. Commit GLSL under `runtime/src/gs/gpu/glsl/` with a regeneration script `scripts/shaders.sh`.
      WGSL stays the source of truth.
- [x] 2.3 `runtime/src/gs/gpu/backend.hpp`: thin interface covering exactly the 2.1 inventory (buffers, binding
      slots, dispatch, draw, submit, present). `backend_wgpu.cpp` = current code moved behind it (PC, unchanged
      behaviour). `backend_gl.cpp` = GL 4.3 core: SSBOs (`glBindBufferBase GL_SHADER_STORAGE_BUFFER`),
      `glDispatchCompute`, `glMemoryBarrier`, `glMapBufferRange` for readback, render pipeline for `hwdraw` via
      VAO + program; `DualSourceBlending` → `GL_ARB_blend_func_extended`; timestamps → `GL_ARB_timer_query` or off.
      Windows build: glad + SDL_GL context. Switch: EGL via SDL2 (`SDL_WINDOW_OPENGL`) with libnx Mesa.
- [x] 2.4 PC validation first: build Windows `ps2run` with `PS2_GPU_BACKEND=gl`, run harness, `compare.py` vs
      `baseline/pc-wgpu/` → must match (zero diff on logos/menu; race ≤ tolerance). Fix until it does.
- [x] 2.5 Switch build with `PS2_GPU` + GL backend. Check Mesa limits at init (`GL_MAX_SHADER_STORAGE_BLOCK_SIZE`,
      `GL_MAX_COMPUTE_WORK_GROUP_*`); clamp as `gpu.cpp` does for wgpu limits (~L3225).
- [x] 2.6 Ryujinx + Hardware (`NV120`, Mesa `20.1.0-rc3` GL 4.3): correctness DONE (2026-11-15: fixed Switch Mesa
      GPU hang by scalarizing `expand16_` + `convert` in `gs_decode.comp`/`gs_raster.comp`/`gs_raster_mip.comp` and
      defaulting `write_buffer_fresh` on `__SWITCH__` to `fresh_mode=2` (`gpu_idle()`); `4500` frames completed with
      0 hangs, `5140` submits, `233396` raster dispatches, `463048` mipmap setups; `frame_00300`/`00600` 0.000% diff
      and `frame_04500` 0.187% diff vs `baseline/pc-gl`). Hardware FPS: FMV `13.3–17.7 fps` (`68.2 ms` GPU raster for
      2x `896x1x1` passes), menu ~3.0–3.6 fps (with frame dumps), in-game race ~1.0–1.3 fps (~717–1033 ms/frame wall);
      IOP `~5.4–6.1 ms`/frame (`PS2_IOP_REGIONS`).
- [x] 2.7 Decision (2026-11-15): **GPU-bound on Switch GL compute (`< 30 fps`: 13.3 fps FMV, ~1 fps in-game race)**
      under Mesa `20.1.0-rc3` (`nouveau` + `gpu_idle()` per fresh buffer write + hundreds of compute dispatches/frame)
      → proceed to **Phase 3 (`deko3d`)** native GPU backend (or `deko3d` command-buffer batching without GL stalls).

## Ryujinx findings (2026-11-15) — read before touching the GL backend or the shaders

What it took to run under Ryujinx 1.3.3 (all in `MC2Recomp/ps2-native-recomp`, all behind `__SWITCH__` / `PS2_GPU_GL`):

- **C++ exceptions crashed Ryujinx** (`Unknown MRS 0xD53B2521`): GCC 16's unwinder holds Guarded-Control-Stack code;
  Ryujinx's JIT rejects `MRS GCSPR_EL0` when it compiles the function. `scripts/nx_nop_chkfeat.py` runs after every
  Switch link and rewrites CHKFEAT/GCSPR/GCSPOPM to what the A57 does anyway. The IPU's setjmp hack was reverted.
- **Mesa's storage-buffer code does not run in Ryujinx** (`Ld`/`St` not implemented, black picture, device lost):
  `scripts/glsl_compat.py` (run by `scripts/shaders.sh`) turns every SSBO into an r32ui buffer image, atomicLoad/
  Store into plain coherent image loads/stores, wraps `main` (no PRET), replaces variable uint `/` `%` (no JCAL) and
  every `switch` (threads vanished after one) with if-chains. `backend_gl.cpp` binds buffer textures as images.
  Same pixels on PC GL; PC GL (`build/gl-mc2-o3`) is a bit faster than WebGPU (12.7 s vs 13.6 s for 1500 frames).
- **Mesa does not wait for image writes before a buffer read**: `read_buffer` keeps its `glFinish`.

Where the time goes in Ryujinx (frame log, `PS2_IOP_TIME`, `PS2_GL_SKIP`; host: RTX 3070 Ti laptop):

| Scene | Ryujinx ms/frame | PC native ms/frame |
|---|---|---|
| logo / intro films | 25–90 | ~9 |
| ESRB + loading, menu | 270–380 | ~9 |

- IOP interpreter: **~24 ms per frame on its own** (615k IOP instructions/frame) → full speed (16.7 ms) is impossible
  in Ryujinx while the IOP is interpreted. Ryujinx's JIT is ~8× slower than PC on this interpreter loop.
- ESRB/menu: the emulation thread is busy only ~40 ms; the rest waits for the GPU. `PS2_GL_SKIP=<raster program>`
  drops 266 → 44 ms. Inside the rasterizer the cost is texture sampling in `plot()` (~160–200 ms/frame). Dispatch
  count, buffer sizes, uploads, readbacks, memory-manager mode, Mesa's NIR path: measured, not the cause.
- Not known yet: why the translated texture-sampling code is that slow (likely Mesa's code for the huge inlined
  raster shader → Ryujinx's shader translator). Real hardware may behave very differently: measure there (1.16).

Debug switches (set in `sdmc:/mc2/env.txt`, one `NAME=value` per line): `PS2_GL_SELFTEST=1` (GPU round trips and
dispatch costs), `PS2_GL_TIMING=1|2` (backend call times), `PS2_GL_SKIP=<program ids>`, `PS2_VRAM_DUMP=<file>@<n>`,
`PS2_IOP_TIME=1`, `PS2_FRAME_LOG=/mc2/frames.log`, `PS2_PRESENT_LOG=1`. Extra arguments: `sdmc:/mc2/args.txt`.
Test loop: `tools/ryujinx_run.sh <nro>`, `tools/ryujinx_compare.py <out.png>`.

Next steps, most promising first:
- [x] N.1 Real Switch (2026-11-15): GL NRO (`build/switch-gl-mc2/ps2run.nro`) ran 900 frames + 4,500-frame full harness on hardware with 0 hangs (Logos 13.3–17.7 fps without dumps / 130–180 ms wall with PPM dumps, menu 276–338 ms, in-game race 717–1033 ms / ~1.0–1.3 fps). GPU-bound under Mesa GL compute → Ryujinx bottleneck confirmed on hardware → Phase 3 (`deko3d`).
- [x] N.2 IOP ahead-of-time translation (2026-11-15). Nearly all IOP time was one AGESND (sound driver) thread polling the
      clock ~12 000×/frame (615k instructions/frame). `PS2_IOP_HIST=<file>` (interpreter) records counts, entry counts
      and IOP RAM; `scripts/iop_recomp.py <file> <out.cpp>` writes regions (basic blocks joined by goto, RAM loads/stores
      inline, HLE import calls done in place so the polling loop never leaves its region); CMake `-DPS2_IOP_REGIONS=<cpp>`
      builds them in; each region compares its code with IOP RAM on entry and otherwise leaves it to the interpreter.
      MC2: `games/mc2/iop/iop_regions.cpp`, 139 regions, 99.5 % of executed instructions. Result: logs and all 75 harness
      images identical to the interpreter, same cycle count; IOP time PC 3.9 → 0.6 ms/frame, Ryujinx 24 → ~8 ms/frame.
      Switches: `PS2_IOP_INTERPRET=1` (off), `PS2_IOP_REGIONS=<from>-<to>` (subset, for bisecting), `PS2_IOP_REGION_TRACE=1`.
- [x] Picture was upside down on the OpenGL path: naga keeps WebGPU's clip-space y flip in blit.vert;
      `glsl_compat.py` removes it. Checked on the PC window (`PS2_GL_SCREEN_DUMP=<file>@<picture>` dumps the backbuffer).
      Ryujinx's backbuffer readback is unreliable (mostly black) — check orientation on hardware.
- [ ] N.3 Raster shader: split texture sampling out of `plot()` (smaller shader, fewer live registers), or try a
      texelFetch (`usamplerBuffer`) path for the read-only bindings 1/3/6 instead of image loads.
- [ ] N.4 deko3d (Phase 3) only after N.1 shows GPU-bound on hardware.

## Phase 3 — deko3d backend (only if 2.7 says GPU-bound)

- [x] 3.1 (done, see HANDOFF.md) `backend_deko3d.cpp` on the same `backend.hpp`: `DkDevice`/queue, memblocks for SSBOs, compute shaders
      compiled from the Phase 2 GLSL with `uam`, `dkCmdBufDispatchCompute`, barriers, swapchain present
      (`dkSwapchain`), render pipeline for hwdraw.
- [x] 3.2 (done: Ryujinx 0.0 % at 300/600/3600; hardware runs clean) Same harness; Ryujinx renders deko3d (NVN) fine for correctness. Hardware fps table again.

- [x] 3.3 30 fps milestone part 1 (2026-11-15): NRO overclock (`host/switch_clocks.*`), lean raster shaders, 8x8 tiles. Handheld 1785/921 MHz: films 33-35, menus 34-42, race 75-78 ms/frame. Race 30 fps not reached: see HANDOFF.md "30 fps milestone" for next options.

## Phase 4 — Polish

- [ ] 4.1 Startup options screen (SDL2-rendered, controller-driven): widescreen, 16:9 HUD fix, 60 FPS patch
      (default off), resolution 1×/2×, P2 controller. Writes `sdmc:/mc2/options.ini`; maps to `toggles.txt`.
- [ ] 4.2 Two controllers → 2P split-screen (`pad2_` bindings from `keys.ini`).
- [ ] 4.3 NRO icon/nacp final, `README-switch.md`: SD layout, how to dump/convert ISO, title-override launch,
      build steps. Release zip = `ps2run.nro` + `toggles.txt` + README, **no** disc data or `BOOT.ELF`
      (user extracts from own ISO; ship a one-line script for that).
- [ ] 4.4 Upstream-friendly diff: all Switch code behind `__SWITCH__`/`PS2_SWITCH`; PC build + autotests 54/55
      unchanged. Re-run Phase 0.7.

## Reference: command cheatsheet

```bash
# PC build (Git Bash)
cd $HOME/Downloads/mc2/MC2Recomp && bash tools/build.sh "$HOME/Downloads/mc2/Midnight Club II (USA).iso"

# Coverage rounds
cd ../ps2-native-recomp && bash scripts/port.sh mc2 "$HOME/Downloads/mc2/Midnight Club II (USA).iso" --native-only

# Switch build (devkitPro msys2 shell)
cmake -S . -B build/switch-mc2 -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DPS2_SWITCH=ON -DPS2_GENERATED=generated/mc2 && ninja -C build/switch-mc2 ps2run.nro

# Deploy to hardware with stdout back
nxlink -s build/switch-mc2/ps2run.nro
```

## Parked: unattended Switch dev loop (designed 2026-11-15, not built)

Console: FW 20.5.0, Atmosphère 1.9.5 → a sysmodule cannot relaunch apps (ns:dev launch calls end at FW 17), so a hard
kill (system closes the app) always needs the user. Design: (1) `mc2dev.nro` resident loader speaking hbmenu's netloader
protocol (UDP "nxboot"/"bootnx" discovery, TCP 28280: name len+name, file len, zlib chunks ≤16 KiB with u32 size, args;
nx-hbmenu common/netloader.c) → saves NRO, envSetNextLoad(it), exits; (2) ps2run calls envSetNextLoad(mc2dev) at start
(normal exit returns there); crash handler + deko3d error callback: crash.txt then appletRequestToRelaunchApplication;
(3) relaunch must land in mc2dev: custom nx-hbloader build defaulting to mc2dev if Atmosphère allows a per-title hbl path,
else mc2dev as what the dev title opens; (4) one spare game as dev title: override_config.ini program_id_0=<id>,
override_key_0=!R; (5) verify with a forced crash (PS2_CRASH_TEST) on hardware.
