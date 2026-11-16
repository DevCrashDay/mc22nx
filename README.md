# mc22nx

A port of **Midnight Club II** (PS2, USA, SLUS-20209) to the **Nintendo Switch 1** as homebrew. It builds on MC2Recomp and the
`ps2-native-recomp` toolkit: the game's code is translated to C++ ahead of time and compiled for the Switch's ARM CPU. The PS2
graphics are drawn with the Switch GPU through deko3d.

## Play it

**→ https://devcrashday.github.io/mc22nx/**

Drop your own ISO of the game on that page. It reads the game's main program from the ISO in your browser (nothing is uploaded),
unlocks the Switch build with it and gives you `mc22nx.zip` for the SD card. Copy your ISO to `sdmc:/mc2/mc2.iso`, then start the
homebrew menu through a game (hold **R** when starting any game) and run **mc22nx**.

## State (November 2026)

| | |
|---|---|
| Boots, menus, films, races | yes, on real hardware (Atmosphère) |
| Race speed, handheld | Los Angeles ~20 ms per frame (~50 fps), Tokyo ~22 ms (~45 fps); target 16.7 ms (60 fps) |
| Known gaps | minimap and some hint texts not drawn by the GPU renderer yet; split-screen (second pad) not working |

The three emulator threads (EE + IOP, vector units, GS/GPU) each run on their own CPU core. The latest numbers per thread, what was
measured and what comes next are in [`docs/HANDOFF.md`](docs/HANDOFF.md); the performance to-do list is
[`docs/performance-plan.md`](docs/performance-plan.md).

## What is in this repository

| Folder | What |
|---|---|
| `ps2-native-recomp/` | the toolkit (MIT, by Samir Nuri and exoteric33) with the Switch port: `cmake/switch.cmake`, deko3d backend (`runtime/src/gs/gpu/backend_deko3d.cpp`), GPU triangle renderer (`runtime/src/gs/gpu/hwr/`), ahead-of-time VU1 programs, IOP idle loop, Switch host code |
| `MC2Recomp/` | the game layer (MIT, by jcoker85): option patches, hub, build scripts |
| `docs/` | findings, plans, the hand-off notes of the port |
| `tools/` | test harness, picture compare, `make_bundle.mjs` (builds the encrypted Switch bundle for the page) |
| `site/` | the GitHub Pages tool (`index.html`); it is published from the `gh-pages` branch together with the bundle |

Nothing taken from the game is in the repository: no disc image, no files extracted from it, no recompiled game code. The page's
bundle is encrypted with a key that only the game's own main program gives.

## Build it yourself

You need your ISO, Windows, Python 3, [devkitPro](https://devkitpro.org) (devkitA64, libnx, deko3d, switch portlibs) and LLVM-MinGW
for the PC tools.

1. Follow `MC2Recomp/README.md` to set up the toolkit and translate the game on the PC (`tools/build.sh "<iso>"`): this creates
   `ps2-native-recomp/games/mc2/` (game files, entry points, VU1 recordings) and `ps2-native-recomp/generated/mc2/`.
2. Compile the VU1 programs: `build/tools/vurecomp.exe games/mc2/vu1 generated/mc2 --vu0-dir games/mc2/vu0`.
3. Translate the IOP's hot code: run the PC build with `PS2_IOP_HIST=games/mc2/iop/capture.txt` through a race, then
   `python scripts/iop_recomp.py games/mc2/iop/capture.txt games/mc2/iop/iop_regions.cpp`.
4. Build the Switch NRO in devkitPro's shell (`C:/devkitPro/msys2/usr/bin/bash.exe -l`, with `export TMP=/tmp TEMP=/tmp`),
   from `ps2-native-recomp/`:
   ```
   cmake -B build/switch-gl-mc2 -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release \
         -DPS2_SWITCH=ON -DPS2_GPU_GL=ON -DPS2_GPU_DEKO3D=ON -DPS2_GENERATED=$PWD/generated/mc2 \
         -DPS2_IOP_REGIONS=$PWD/games/mc2/iop/iop_regions.cpp
   cmake --build build/switch-gl-mc2 -j 8
   ```
5. SD card: `switch/mc22nx.nro` (`build/switch-gl-mc2/ps2run.nro`), `mc2/mc2.iso`, `mc2/BOOT.ELF` (`games/mc2/patched/SLUS_202.09`),
   `mc2/patches.txt`, `mc2/toggles.txt`. Optional `mc2/env.txt` (`NAME=value` lines) and `mc2/args.txt` (extra arguments).

`docs/HANDOFF.md` has the full build, test and debugging commands (PC OpenGL build with the same GPU renderer, Switch runs over
nxlink, the per-thread timing switches).

## Credits

- [ps2-native-recomp](https://github.com/exoteric33/ps2-native-recomp) — Samir Nuri, exoteric33 (MIT)
- [MC2Recomp](https://github.com/jcoker85/MC2Recomp) — jcoker85 (MIT)
- [sse2neon](https://github.com/DLTcollab/sse2neon) (MIT), devkitPro / libnx / deko3d
- Midnight Club II © Rockstar Games. This project is not affiliated with Rockstar or Nintendo; you need your own copy of the game.
