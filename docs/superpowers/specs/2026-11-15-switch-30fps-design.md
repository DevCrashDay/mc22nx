# Midnight Club II on Switch: 30 fps docked (overclock + faster raster) — design

Date: 2026-11-15. Status: draft for approval. Context: `HANDOFF.md` (deko3d backend, measurements), `to-do.md`, `plan.md`.

## Intent

The Switch port (deko3d backend, `build/switch-gl-mc2` with `-DPS2_GPU_DEKO3D=ON`) runs correctly but slowly on the user's
original 2017 Switch (Erista, Tegra X1): films ~58 ms/frame (CPU-bound), menus 100–128 ms, race ~245 ms (~4 fps, GPU-bound).
The user's end goal is 60 fps docked; this project is the first milestone: **30 fps docked** (the game's own rate, no
60 fps patch), reached by overclocking from inside the NRO and by making the GPU raster shader do less wasted work.

## Goals

1. **Clocks set by the NRO itself** (no sys-clk), planned for an **overclock kit** (user's decision): the user installs a kit
   (Atmosphère/Hekate boot-time patches that extend the clock tables); the NRO then requests above-stock rates itself and
   falls back to the stock maximums when no kit is present. Kept applied while the game runs (the system resets clocks on
   dock/undock and other profile changes), restored to the previous values on normal exit and from the crash handler.
2. **Faster raster shaders** (`gs_raster.comp`, `gs_raster_mip.comp`; the two take ~87 % of race GPU time) with **pixels
   unchanged**.
3. **Measured result**: every scene of the harness run at ≤ 33.3 ms/frame docked.

## Success criteria

- Hardware run with `tools/harness.sh`'s inputs to frame 3700, docked, no frame dumps: average wall time per frame
  ≤ 33.3 ms in each section (logos/films 0–1100, menus 1100–2900, race 3000–3700) from `PS2_IOP_TIME` lines; no crash.
- PC OpenGL build: all 75 harness images 0 diff vs `baseline/pc-gl` (`tools/compare.py --tol 0 --max-pct 0`).
- Ryujinx deko3d run: frames 300/600/3600 0.0 % vs baseline (1800 is the known memory-card dialog difference).
- After the NRO exits (normally or via the crash handler) the console is back at its previous clocks.

## Non-goals

- 60 fps and the `[60 FPS]` patch (next milestone).
- Installing or configuring the overclock kit itself (the user does that; the NRO only requests rates and reads back what
  took effect). RAM speed: set by the kit at boot, not changed by the NRO (only logged).
- Porting the newer toolkit's experimental `hwdraw` (hardware triangle) path — revisit only if this plateaus.
- A handheld performance target (handheld must work, not be fast).

## Anti-goals

- Any visual difference from the PC baseline.
- Clocks left raised after the game ends, or overheating/instability from them.
- Breaking the OpenGL Switch build or the PC builds.

## Constraints

- Toolkit base stays v0.0.4 + `MC2Recomp/patches/ps2-native-recomp.patch` (the newer toolkit does not take the patch).
- Shaders: WGSL is upstream's source; our GL/deko3d GLSL is naga output + `scripts/glsl_compat.py` + hand fixes
  (`gs_decode/raster*.comp`, see HANDOFF) + `scripts/glsl_raster_lean.py` + `scripts/glsl_dk.py`. Raster changes go into
  `glsl_raster_lean.py` (applied at build time for GL and deko3d), so both PC GL (pixel check) and Switch get them.
- Switch builds: devkitPro msys shell, `make -C build/switch-gl-mc2 ps2run_nro`; only runtime/window code rebuilds.
- Hardware runs need the user to open netloader each time; keep the number of runs low (combine measurements per run).
- Nothing is committed unless the user asks.

## Approaches considered

Clock source: stock limits only vs. planning for an overclock kit → **kit** (user's decision), with automatic fallback to
stock when the kit is absent.

1. **Overclock in the NRO + speed up the compute rasterizer** — chosen. Cheap certain gain from clocks; the rasterizer work
   targets the waste the timing runs measured; pixels stay exact. Gain of the rasterizer part uncertain (estimate 2–5×).
2. Overclock + hardware triangle drawing on deko3d (port of upstream's experimental `hwdraw`) — highest ceiling, largest job,
   visual differences likely; kept as the fallback if (1) plateaus short of 30 fps.
3. Overclock only — too small (GPU only ~1.2×).

## Design

### Part A — clocks (`host/main.cpp` or a small `host/switch_clocks.cpp`, Switch only)

- libnx `clkrst` (firmware ≥ 8.0) with `pcv` fallback for older firmware: open sessions for `PcvModuleId_CpuBus`,
  `PcvModuleId_GPU`, `PcvModuleId_EMC`; read current rates first (to restore).
- Targets (my call, overridable): with a kit, CPU 2091 MHz and, docked, GPU 1228.8 MHz — common Erista kit values, below the
  kits' upper ends to keep heat and stability margins; without a kit the stock maximums CPU 1785 / GPU 921.6.
  Kit detection = request the kit target, read the rate back with `clkrstGetClockRate`; if it did not take, request the stock
  maximum instead. The log states which applied (`[clocks] cpu 2091 MHz (kit) gpu 1228 MHz (kit) emc 1862 MHz`).
- Docked only for the GPU raise (`appletGetOperationMode() == AppletOperationMode_Console`). Handheld: CPU raised, GPU left
  at the system's value (my call: handheld power/heat limits; the goal is docked).
- Overrides in `sdmc:/mc2/env.txt`: `PS2_CLOCKS=0` (leave clocks alone), `PS2_CLOCKS=<cpu MHz>,<gpu MHz>` (exact request,
  same read-back fallback).
- Re-apply: a check every ~1 s from an existing per-frame/vsync hook (or a tiny thread) comparing the current rate and
  setting it again if the system changed it; also on operation-mode change.
- Restore on exit (`atexit`/end of `main`) and in `__libnx_exception_handler` before `svcExitProcess`.
- `sdmc:/mc2/env.txt` switches: `PS2_CLOCKS=0` (leave clocks alone), `PS2_CLOCKS=cpu,gpu` in MHz (testing), log line with
  the rates before/after.
- Ryujinx: the services exist as stubs; must not crash (check once).

### Part B — raster shader work reduction

Today each 16×16 tile is one work group; every one of its 256 threads walks the tile's whole primitive list, loads the
primitive, tests its bounding box and edges, and runs `shade_at` only if its own pixel is covered.

1. **Measure first** (one hardware or Ryujinx/PC run; counts are the same on PC): behind an env switch, atomic counters in a
   debug buffer per frame: primitives visited per tile, primitives whose bbox misses the whole tile row/column of a thread,
   thread-primitive pairs that reach `shade_at`, pixels written. This says how much work is pure walking.
2. **Then pick the cut by the numbers** (decision recorded in HANDOFF.md), candidates:
   - work-group early-out: per primitive, a cheap shared-memory vote whether any thread of the tile is covered; skip the
     rest of the per-primitive work for the whole group when none is;
   - finer binning on the CPU side (`gpu.cpp` builds per-tile lists): 8×8 sub-tiles or a per-primitive pixel-coverage
     mask so threads skip primitives that miss their quarter;
   - move per-primitive setup (edge equations, attribute deltas) out of the per-thread path into shared memory, computed
     once per primitive per group (same idea as the shared Setup, which already cut registers 255 → 118).
3. Each change: register count from the `.dksh` header (byte 32), PC 75-image check, then one hardware timing run
   (`PS2_DK_STATS=1`, `PS2_DK_TIMING=1`, `PS2_IOP_TIME=1`).

### Part C — CPU side check

At 2091 MHz (kit) the films' emulation thread (~58 ms at 1020 MHz) should drop to ~28 ms; at stock 1785 to ~33 ms —
borderline. If films/menus stay
above 33.3 ms after A and B, measure where the emulation thread spends its time (existing `PS2_IOP_TIME` + a coarse
EE/VU/IPU split) and treat it as a follow-up; not designed here.

## Risks

- 30 fps may need more than the rasterizer cuts give (race needs ~7× after the GPU clock); the measurement in B.1 tells
  early. Fallback: approach 2.
- Kit clocks on Erista (2091 / 1228.8) run hotter than stock and depend on the individual chip: instability shows as
  crashes or freezes, not wrong pixels. Mitigation: conservative defaults, `PS2_CLOCKS` override, the read-back log, and the
  restore on exit/crash. Without a kit the NRO gets the stock maximums (1785 / 921.6), which are in Nintendo's own tables.
- Expected gain: GPU 768 → 1228.8 MHz is ~1.6× (stock 921.6: ~1.2×); CPU 1020 → 2091 ~2× (stock 1785: ~1.75×). Race still
  needs roughly 4–5× from Part B with the kit (~7× without).
- Ryujinx ignores clocks, so all speed numbers come from hardware.

## Left to the builder

- Exact place of the clock re-apply hook and the restore-on-exit wiring.
- Counter layout for B.1 and how it is printed.
- Which of the B.2 candidates, in what order, after the numbers.
