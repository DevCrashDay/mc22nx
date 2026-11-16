# Lessons from other PS2 → Switch ports (2026-11-16)

Two projects reviewed for our Midnight Club II port (deko3d GS backend + the GPU triangle renderer, `PS2_HWR=1`).

## jak-project-nx (OpenGOAL Jak 1–3 on Switch) — closely comparable

What it is: OpenGOAL's decompiled/recompiled Jak games running natively on ARM64; its desktop OpenGL renderer was
moved to deko3d. Same toolchain (devkitPro/libnx, uam offline shaders), same launch mode (title override, full heap), no
SDL video (renderer on `nwindowGetDefault()`). Sources read: `DEKO3D_MIGRATION_PLAN.md`, `PERF_PLAN_NEXT_AGENT.md`,
`performance-improvement.md`, `SWITCH_PORT_SESSION_NOTES.md` (first part).

| Their finding | Our state | Action |
|---|---|---|
| 2 swapchain images stalled the GPU on display release; 3 fixed it | we used 2 | **applied**: `kSwapImages = 3` |
| Unbounded fence waits froze the render thread silently; bounded waits with a log line | we waited `-1` | **applied**: `wait_fence()` logs every 1 s |
| SDL pad polling after libnx's HID exit → `0x1159`; quit the gamepad subsystem in an `atexit` first | not handled | **applied** (`host/window.cpp`) |
| `dkQueueSignalFence(..., true)` only flushes caches; work goes out with `dkQueueFlush` | we already flush | — |
| Uploads on the present queue waited behind vsync (~16.6 ms per copy); a separate upload queue → 0.05–0.3 ms | one queue; our uploads/fetches are recorded with the draws | measure first (our CPU-profile build); a second queue is the fix if acquire/vsync shows up |
| CPU per deko3d draw ~0.1–0.2 µs vs GL ~3.8 µs; their bottleneck stayed CPU-side render submission (45–67 ms), GPU overclock changed nothing | ours: GPU busy only ~21 ms of a ~70 ms race picture | consistent: measure the GS-thread CPU (pending run), then cut per-shape CPU work |
| Pipeline state from a static table, bound by id; state cached | we bind blend/depth/mask only on change | — |
| Textures with full mip chains; single-mip textures slowed the GPU badly | we sample level 0 only (65 % of race shapes use mipmaps) | stage 2/3 item: mip levels in the texture cache |
| Delete/free GPU resources a few frames later (in-flight frames) | `destroy_image` waits for the GPU (flush) | fine for now (rare); deferred release if eviction shows in profiles |
| Grow-only buffers/rings, coalescing allocators | fixed rings | — |
| newlib's SD layer (fsdev) is not thread-safe: wrap file I/O in one lock | disc read-ahead has its own lock; logs/memory card/disc from several threads | **todo**: one recursive lock around SD file I/O |
| Crash addresses: print a symbol anchor, offset = pc − anchor + nm address | done (`handler +0x...`) | — |
| No stacked software limiter on top of vsync: swap interval 2 for 30 fps | the runtime paces itself + swap interval 1 | later, with frame pacing work |
| CPU boost mode only for loads (it lowers the GPU clock) | we set clocks ourselves | — |
| Core pinning: no measurable gain for them | EE pinned to core 0 | — |

## hp2_godot (Need for Speed: Hot Pursuit 2, PS2) — different approach

A reverse-engineered reimplementation: C++ game core (race logic, physics, AI, menus) with Godot doing graphics/input,
PC only (Windows/Linux/macOS, Vulkan/D3D12/Metal); no Switch target. Not applicable to a recompilation port: it replaces
the PS2 renderer entirely instead of emulating the GS. Takeaway only: separating the game core from a native renderer
is what ultimately removes GS-emulation cost — our GPU triangle renderer is the closest step in that direction.

## ps22switch (DevCrashDay, Tekken 4) — same toolkit, Vulkan/NVK first, now moving to deko3d

A fork of ps2-native-recomp like ours. GS: upstream's WebGPU renderer on a WebGPU-on-Vulkan shim over Mesa NVK, plus a hardware
triangle path (`hw.wgsl`, `PS2_GS_HW=1`). Fights 21–30 fps, menus 60 (CPU 1785 / GPU 921 / memory 1600 MHz). Their own measurements:
NVK costs 18.6 ms render-thread time vs 2.8 ms on deko3d (USF4 port), so they are porting to deko3d, the path we already took.

| Their finding | Our state | Action |
|---|---|---|
| Memory (EMC) 1600 MHz with CPU/GPU overclock | our log: `emc 1600 MHz` | — same |
| ~40 % of GPU time not covered by timestamps = barriers / pass load-store; one barrier tens of µs on Tegra | our race: ~139 batches per picture split by "target read", each ending in a Fragments barrier | candidate: fewer batch splits (see the per-thread profile first) |
| `emit_pre` had an O(n²) overlap test (CLUT loads vs uploads): 343 M tests/s, 10× cut by one forward pass over merged intervals | ours has the same nested loops (reorder + grouping); in GPU-renderer mode the lists are short because each texture fetch flushes | measure; port their interval pass if the GS thread shows it |
| CLUT straight from the just-uploaded palette bytes removes most upload+CLUT dispatches | race: ~670 compute dispatches per picture (uploads, CLUT loads, fetches) | candidate (GPU + GS-thread) |
| Present: SDL readback/upload costs 6.4 ms per frame | we present from deko3d directly | — done |
| SDL owns the window's EGL surface; a native swapchain only works without it (3 buffers) | no SDL window under deko3d, 3 swap images | — done |
| naga GLSL rewrites z as `z*2-w` for Vulkan; deko3d depth 0..1 | our shaders are hand-written GLSL for deko3d | — not affected |
| VU programs not seen on PC are interpreted (coverage = speed); upstream 2026-11 adds a VU0 AOT generator and `vu_jit` | 58 compiled VU1 programs registered; interpreted share in the race unknown | check with `PS2_VU_COUNT`/`PS2_VU_TIME` if the VU thread is the bottleneck |
