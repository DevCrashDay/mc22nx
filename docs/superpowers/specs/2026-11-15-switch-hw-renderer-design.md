# Midnight Club II on Switch: GPU triangle renderer (hardware rasterization) — design

Date: 2026-11-15. Status: draft for approval. Follows `2026-11-15-switch-30fps-design.md` (overclock + raster work,
done; results in `HANDOFF.md` "30 fps milestone").

## Intent

On the user's 2017 Switch (handheld, CPU 1785 / GPU 921.6 MHz set by the NRO) the race runs at 75–78 ms/frame (~13 fps).
The GPU is the limit: our GS emulation draws every pixel of every shape in one big compute shader (the "painter",
`gs_raster*.comp`), ~7300 shapes and ~8M bounding-box pixels per race frame. The goal of this project is the race at
≤ 33.3 ms/frame (30 fps, the game's own rate) by drawing the GS's triangles and sprites with the Switch GPU's
fixed-function hardware (rasterizer, depth test, blender) through deko3d, the way PC PS2 emulators' hardware renderers do.

**The user's decision on exactness: "looks the same is enough".** The picture may differ from the PC reference in tiny
rounding and edge details, but must not show wrong colours, missing effects, flicker or garbage.

## Goals

1. A GPU renderer as the **main** drawing path on the Switch build: the game's frame and depth buffers live as GPU images;
   triangles, triangle strips/fans and sprites are drawn into them by deko3d graphics pipelines.
2. Race ≤ 33.3 ms/frame average (frames 3000–3700 of the harness inputs) on hardware, handheld, at the NRO's clocks.
   Menus and films no slower than today (34–42 / 33–35 ms).
3. Everything in the harness run (logos, films, menus, race) looks the same as the PC reference to a player.
4. The painter stays in the build as the fallback (shapes the hardware path cannot do, and a switch to turn the GPU
   renderer off).

## Non-goals

- Pixel-identical output (explicitly given up by the user).
- Other games; only what MC2 needs, decided by measurement (stage 0).
- Higher internal resolution / upscaling, 60 fps — later milestones.
- The PC wgpu build; upstream's `hwdraw.cpp` port (approach 1, rejected below).

## Anti-goals

- Wrong colours, missing effects (glows, reflections, fog, shadows, HUD), flicker, garbage pixels.
- Memory that grows over time (upstream's card path crashed with "Not enough memory left" after seconds in one game).
- A renderer that is faster per shape but loses it again in syncing with the painter on every frame.
- Breaking the painter path (`PS2_HWR=0` must still give today's picture).

## Constraints

- Toolkit base v0.0.4 + MC2 patch; Switch backend deko3d (`runtime/src/gs/gpu/backend_deko3d.cpp`, `backend.hpp`);
  the GS front end (`runtime/src/gs/gs.cpp`) calls `gpu::draw(State, Vertex*, count)`, `image_begin/image_data`
  (EE→GS uploads), `local_copy`, `load_clut`, `texflush`, `download(_async)` (GS→EE readbacks), `present/display`,
  `read_state/write_state` (quick saves) — these are the hook points (`gpu.hpp`).
- Shaders: GLSL compiled by `uam` at build time (as today); deko3d has no pipeline objects (state is set per draw),
  so per-draw state changes are cheap on the GPU side but cost CPU time to encode.
- CPU budget: the emulation thread uses ~15 ms of a race frame today; the new per-draw work on the CPU must keep the
  frame under 33.3 ms (films are already CPU-bound at 33–35 ms and must not get slower).
- Testing: Ryujinx runs deko3d (`tools/ryujinx_run.sh`, ~4 min per 3700 frames, frame dumps); hardware runs need the
  user to open netloader each time. Nothing is committed unless the user asks.

## Approaches considered

1. Port upstream's experimental card path (`ps2-native-recomp/runtime/src/gs/gpu/hwdraw.cpp`, ~4800 lines, wgpu, newer
   toolkit) — rejected: painter and card alternate on the same pictures, and the syncing/copying between them made it
   5–17 % slower than the painter on PC; it targets a different API and toolkit version.
2. **GPU renderer as the main path, painter as fallback (PCSX2-style)** — **chosen by the user**: frame/depth buffers
   are GPU images; copies back to emulated GS memory only on demand; highest ceiling.
3. Only the race's dominant shape classes on the GPU, the rest on the painter — rejected: same per-switch syncing cost
   as approach 1, and race shapes of different classes interleave.

## Design

### Overview

A new module `runtime/src/gs/gpu/hwr/` (Switch/deko3d first) sits between `gpu::draw` and the painter:

- **Target cache**: GPU images for GS frame buffers keyed by (FBP, FBW, PSM) and depth buffers keyed by (ZBP, ZPSM),
  RGBA8 colour (the GS's 32/24/16-bit formats converted on write-back) and a 32-bit depth image (D32 float or
  D24S8, builder's call by what deko3d offers; GS depth is 16/24/32-bit integer). Created on first draw, sized to the
  frame (MC2: 640×448), reused across frames.
- **Texture cache**: GS textures (TBP, TBW, PSM, TW, TH, CLUT state) decoded into RGBA8 GPU images with the existing
  decode/CLUT compute shaders (`gs_decode`, `gs_load_clut`); invalidated when `image_data`/`local_copy` writes overlap
  them. A texture whose address is a cached render target is sampled **from the target image directly** (MC2 draws its
  car reflection map every frame and reads it as a texture).
- **Drawing**: each `gpu::draw` batch becomes vertices (screen position from the GS's 12.4 fixed point, Z, colour, UV/STQ,
  fog) in a per-frame vertex ring, drawn with one deko3d draw per state change. State → deko3d: depth test/write (ZTST,
  ZMSK) → depth state; blending (ALPHA register A/B/C/D) → hardware blend where the formula maps, else the fragment
  shader does it with the destination read from the target (frame-buffer fetch is not available: use a copy of the
  target or "blend in shader" only for the few modes that need it — stage 0 decides); alpha test (ATST, AFAIL) and
  texture function (TFX, TCC), fog, colour clamp in an uber fragment shader with state in a uniform buffer. Scissor →
  deko3d scissor. Sprites → two triangles. Lines/points → painter fallback unless stage 0 shows MC2 uses them often.
- **Write-back on demand**: a target is converted back into emulated GS memory (swizzled, in its PSM) only when
  something reads that memory: `download`/`download_async` (EE readback), `local_copy` from it, the painter drawing
  into or texturing from it (fallback), `read_state` (quick save), or `display` of an address that is not a cached
  target. A compute shader does the conversion (the painter's address functions in GLSL exist and can be reused).
  Conversely, when the EE uploads into a cached target's memory or the painter draws into it, the target is reloaded
  from GS memory before the next GPU draw.
- **Present**: when the displayed frame buffer (DISPFB) is a cached target, the blit reads the target image directly
  (no write-back).
- **Fallback**: per shape, a classification (like upstream's) says GPU or painter. A painter shape forces write-back of
  the targets it touches, the painter draws, then the targets are marked stale (reloaded on next GPU use). Stage 0
  measures how often this happens in MC2; the design assumes it is rare in the race (if it is not, the fallback set is
  shrunk by moving more features into the uber shader).
- **Switch**: `PS2_HWR=0` turns the GPU renderer off (painter only, today's behaviour); default on in the deko3d build
  once stage 3 passes.

### Stages (each ends with a checkable result)

0. **Measure** (PC GL build, counters only, no drawing change): per race frame, shapes by primitive type, texture
   source (EE texture / render target / none), blend formula, alpha test, depth mode, fog, frame/texture PSMs,
   texture sampling features (mipmapping, bilinear/nearest, STQ perspective, REGION_CLAMP/REGION_REPEAT wrap), and how
   many would need the painter under the rules above — counted by shapes **and** by bounding-box pixels (the painter
   cost). Also the sync traffic that would force write-back/reload: per-frame count and size of `download(_async)`,
   `local_copy`, `image_data` into memory overlapping a would-be target, `display` of a non-target address, textures
   read from target memory. Output decides the fallback list and whether in-shader blending is needed. Recorded in
   `HANDOFF.md`.
   **Stop rule (my call):** if shapes needing the painter are > 10 % of the race's bounding-box pixels, or the sync
   traffic implies a write-back/reload every frame of a full target, stop after stage 0 and report to the user with the
   numbers and a reduced-scope proposal; do not start stage 1 without their go-ahead.
1. **Untextured + textured triangles/sprites into a GPU target, shown on screen**, no blending or depth: the race
   geometry appears with the right shapes and textures in Ryujinx (frame dump of the target vs PC reference by eye).
2. **Depth, blending, alpha test, fog, texture function**: the race looks the same as the PC reference in Ryujinx and on
   hardware; hardware timing run.
3. **Render-target textures, write-back on demand, painter fallback, EE uploads into targets, quick saves**: the full
   harness (logos, films, menus, race) looks the same; no memory growth over a 3700-frame run (memory counter logged);
   hardware timing vs the goal.

### Decisions made for the builder (my calls)

- **Texture sampling in the uber shader, not fallback**: nearest/bilinear via the hardware sampler where the wrap mode
  maps (REPEAT/CLAMP), REGION_CLAMP/REGION_REPEAT and any unmappable case via `texelFetch` + manual filtering in the
  shader; STQ perspective via interpolated S,T,Q and divide in the shader; mipmapping only if stage 0 shows the race
  uses it (then the decoded levels go into the texture image's mip chain).
- **Memory**: the NRO runs in title-override mode (3285 MB heap). Caps: render/depth targets 64 MB, texture cache
  256 MB with least-recently-used eviction, vertex ring 16 MB per frame in flight; a `[hwr-mem]` line every 300 frames
  with what is held (checked flat over a 3700-frame run).
- **If 33.3 ms is missed**: the GPU renderer becomes the default anyway when it is faster than the painter in every
  harness section and passes the look check; work then continues on the measured limiter (CPU batching or GPU) and is
  reported, not silently stopped.
- **Hardware runs**: batched at stage ends (each costs the user a netloader press); Ryujinx frame time is used only as
  a CPU-side proxy, never as the speed result.

### "Looks the same" check

No pixel-identical gate. For each harness image (Ryujinx frame dumps, and on hardware a screen capture switch the
builder adds to the deko3d present path, like `PS2_GL_SCREEN_DUMP`): (a) side-by-side + difference image with the PC
reference (`baseline/pc-gl`, the PC painter output), (b) a metric over the full frame — share of pixels differing by
more than 24 in any channel — flagged above 2 % for a look by eye (my call: tiny rounding stays under that, a missing effect or wrong colour does not), (c) the user's eye on the
real Switch during a race.

## Risks

- **Unknown fallback share**: if many race shapes need the painter (destination-alpha tests, unusual blends), syncing
  eats the gain. Stage 0 measures before any renderer code is written; the stop rule: if > 10 % of race shapes would
  fall back and cannot be moved into the shader, report before stage 1.
- **CPU cost per draw** on the A57 (state conversion, vertex conversion for ~7300 shapes/frame): measured in stage 1;
  batching by state is the lever.
- **GS feedback effects** (render-to-texture, the frame read back as a texture, glow passes): the target cache must
  catch MC2's addresses; stage 0 lists them.
- **Depth precision** differences (GS integer Z vs GPU depth) could cause z-fighting flicker; mitigated by a 32-bit
  float depth and exact Z conversion.
- **Effort**: multi-week. Each stage is useful on its own as a measurement; the goal may still be missed if the CPU
  side becomes the limit.

## Left to the builder

- Exact file split inside `runtime/src/gs/gpu/hwr/`, vertex format, uber-shader state layout.
- Depth image format (by deko3d support), how targets are sized beyond MC2's frame.
- Classification details beyond the stage-0 results; the write-back shader structure.
