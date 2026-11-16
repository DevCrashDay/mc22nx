// GS backend on deko3d (Switch only): the same compute shaders as the OpenGL backend (glsl/*, storage buffers again via
// scripts/glsl_dk.py, compiled by uam at build time), without Mesa in between.
//
// Ordering model (mirrors what gpu.cpp expects from wgpu/GL):
// - every CPU write goes to a staging ring and a GPU copy into the target buffer, recorded in the command stream, so it lands
//   after the work recorded before it and before the work recorded after it, with no CPU wait;
// - every dispatch is followed by a barrier (as the GL backend's glMemoryBarrier);
// - a CPU read submits, waits for a fence that flushes the GPU's caches, then reads the memory.
#include "backend.hpp"

#include <deko3d.h>
#include <switch.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define PS2_DK_SHADERS(X) \
    X(Upload) X(CopyRead) X(CopyWrite) X(LoadClut) X(Display) X(Raster) X(Resolve) X(Snapshot) X(Decode) X(RasterMip) \
    X(BlitVert) X(BlitFrag) X(OverlayVert) X(OverlayFrag) X(HwrVert) X(HwrFrag) X(TexBlitFrag) X(TexFetch)
#define PS2_DK_EXTERN(n) extern const unsigned char kDk##n[]; extern const unsigned kDk##n##_size;
PS2_DK_SHADERS(PS2_DK_EXTERN)
#undef PS2_DK_EXTERN

namespace ps2::gs::gpu::be {

namespace {

constexpr int kSegments = 4;
constexpr std::uint32_t kSegmentBytes = 4u << 20;
constexpr std::uint32_t kStageBytes = 64u << 20;
constexpr std::uint32_t kSurfaceW = 1280, kSurfaceH = 720;
constexpr int kSwapImages = 3;  // with 2 the GPU can stall on the display releasing a picture (jak-project-nx)

// startup steps on stderr (a crash before the first picture otherwise leaves nothing to go on)
void step(const char *what) {
    std::fprintf(stderr, "[dk] %s\n", what);
    std::fflush(stderr);
}

std::uint32_t align_up(std::uint32_t v, std::uint32_t a) { return (v + a - 1) & ~(a - 1); }

DkDevice g_dev{};
DkQueue g_queue{};
DkMemBlock g_code{};
DkMemBlock g_cmdmem{};
DkCmdBuf g_cmd{};
DkFence g_seg_fence[kSegments]{};
bool g_seg_used[kSegments]{};
int g_seg = 0;
bool g_recorded = false;     // the open segment holds commands
bool g_after_copy = false;   // copies since the last barrier: wait for them before shaders read
bool g_after_work = false;   // dispatches/draws since the last full barrier: wait for them before a copy

DkMemBlock g_stage{};
std::uint8_t *g_stage_cpu = nullptr;
DkGpuAddr g_stage_gpu = 0;
std::uint32_t g_stage_head = 0;

struct Buf {
    DkMemBlock mem{};
    DkGpuAddr addr = 0;
    std::uint8_t *cpu = nullptr;
    std::uint32_t size = 0;
    bool cached = false;
};
std::vector<Buf> g_bufs(1);  // id 0 = none

std::vector<DkShader> g_shaders(1);  // pipeline id = index

struct BindingSet {
    Buffer buffers[8]{};
    int count = 0;
    int dynamic_slot = 2;
    std::uint32_t dynamic_size = 256;
} g_bindings;

Buffer g_blit_scratch{}, g_blit_ubo{};
Buffer g_overlay_buf{}, g_overlay_rect{};
std::size_t g_overlay_cap = 0;
std::uint32_t g_shader_blit_vert = 0, g_shader_blit_frag = 0, g_shader_ov_vert = 0, g_shader_ov_frag = 0;
std::uint32_t g_shader_hwr_vert = 0, g_shader_hwr_frag = 0, g_shader_tex_blit_frag = 0, g_shader_texfetch = 0;

DkMemBlock g_fb_mem{};
DkImage g_fb[kSwapImages]{};
DkSwapchain g_swapchain{};
bool g_has_surface = false;

// PS2_DK_STATS=1: every 5 s, what went through the backend and how long the CPU waited for the GPU
struct Stats {
    std::uint64_t submits = 0, dispatches = 0, copies = 0, copy_bytes = 0, waits = 0, presents = 0, wraps = 0;
    std::uint64_t hwr_batches = 0, hwr_draws = 0;  // the GPU renderer: hwr_draw calls and draw calls in them
    double wait_ms = 0, blocked_ms = 0;  // blocked: CPU stuck on a busy command segment or a busy swapchain picture
    double gpu_ms[32]{};                    // PS2_DK_TIMING=1: GPU time per shader (pipeline id)
    std::uint64_t gpu_n[32]{};
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    void tick() {
        static const bool on = std::getenv("PS2_DK_STATS") != nullptr;  // read on first use: env.txt is loaded after static init
        if (!on) return;
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds(5)) return;
        last = now;
        std::fprintf(stderr, "[dk] 5 s: %llu presents, %llu submits, %llu dispatches, %llu copies (%.1f MB), %llu waits (%.1f ms), %llu ring wraps\n",
                     (unsigned long long)presents, (unsigned long long)submits, (unsigned long long)dispatches, (unsigned long long)copies,
                     copy_bytes / 1048576.0, (unsigned long long)waits, wait_ms, (unsigned long long)wraps);
        if (hwr_batches) std::fprintf(stderr, "[dk] 5 s: GPU renderer %llu batches, %llu draw calls (shader 30 below = their GPU time)\n",
                                      (unsigned long long)hwr_batches, (unsigned long long)hwr_draws);
        std::fprintf(stderr, "[dk] 5 s: CPU blocked on the GPU %.1f ms%s\n", blocked_ms,
                     barrier_test ? (barriers_off ? " (barriers between dispatches OFF: test only)" : " (barriers on)") : "");
        if (barrier_test) barriers_off = !barriers_off;
        for (int i = 0; i < 32; ++i)
            if (gpu_n[i]) std::fprintf(stderr, "[dk] 5 s: shader %d: %llu dispatches, %.1f ms GPU\n", i, (unsigned long long)gpu_n[i], gpu_ms[i]);
        const bool off = barriers_off;
        *this = Stats{};
        barriers_off = off;
        last = now;
    }
    // PS2_DK_BARRIER_TEST=1: every other 5 s window without the barrier after each dispatch, to measure what the barriers
    // cost (the picture may be wrong in those windows)
    bool barrier_test = std::getenv("PS2_DK_BARRIER_TEST") != nullptr;
    bool barriers_off = false;
} g_stats;

const Buf &buf(Buffer b) { return g_bufs[b.id < g_bufs.size() ? b.id : 0]; }

void on_debug(void *, const char *context, DkResult result, const char *message) {
    std::fprintf(stderr, "[dk] %s: result %d: %s\n", context ? context : "?", static_cast<int>(result), message ? message : "");
    if (result != DkResult_Success) {
        std::fflush(stderr);
        std::abort();
    }
}

void on_cmd_out_of_memory(void *, DkCmdBuf, size_t) {
    std::fprintf(stderr, "[dk] command segment full (%u bytes)\n", kSegmentBytes);
    std::fflush(stderr);
    std::abort();
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Waits for a fence in 1 s steps and says so when the GPU takes longer (a hang then shows in the log).
void wait_fence(DkFence *f, const char *what) {
    for (int seconds = 1; dkFenceWait(f, 1'000'000'000) == DkResult_Timeout; ++seconds) {
        std::fprintf(stderr, "[dk] GPU still busy after %d s (%s)\n", seconds, what);
        std::fflush(stderr);
    }
}

void open_segment() {
    if (g_seg_used[g_seg]) {
        const auto t0 = std::chrono::steady_clock::now();
        wait_fence(&g_seg_fence[g_seg], "command segment reuse");
        g_stats.blocked_ms += ms_since(t0);
    }
    dkCmdBufClear(g_cmd);
    dkCmdBufAddMemory(g_cmd, g_cmdmem, static_cast<std::uint32_t>(g_seg) * kSegmentBytes, kSegmentBytes);
    g_recorded = false;
}

// Sends what is recorded; with wait, also waits until the GPU has done it and its caches are in memory.
void flush(bool wait) {
    if (!g_recorded && !wait) return;
    if (g_recorded) {
        dkQueueSubmitCommands(g_queue, dkCmdBufFinishList(g_cmd));
        ++g_stats.submits;
    }
    dkQueueSignalFence(g_queue, &g_seg_fence[g_seg], true);
    dkQueueFlush(g_queue);
    g_seg_used[g_seg] = true;
    if (wait) {
        const auto t0 = std::chrono::steady_clock::now();
        wait_fence(&g_seg_fence[g_seg], "flush and wait");
        ++g_stats.waits;
        g_stats.wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    g_seg = (g_seg + 1) % kSegments;
    open_segment();
}

void before_shader_work() {
    if (g_after_copy) {
        dkCmdBufBarrier(g_cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Shader);
        g_after_copy = false;
    }
    g_recorded = true;
}

void record_copy(DkGpuAddr src, DkGpuAddr dst, std::uint32_t bytes) {
    if (g_after_work) {
        dkCmdBufBarrier(g_cmd, DkBarrier_Full, 0);
        g_after_work = false;
    }
    dkCmdBufCopyBuffer(g_cmd, src, dst, bytes);
    g_after_copy = true;
    g_recorded = true;
    ++g_stats.copies;
    g_stats.copy_bytes += bytes;
}

// CPU data to GPU memory, ordered in the command stream
void upload(DkGpuAddr dst, const void *data, std::size_t bytes) {
    const auto *p = static_cast<const std::uint8_t *>(data);
    while (bytes) {
        const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(bytes, kStageBytes / 2));
        if (g_stage_head + n > kStageBytes) {  // the ring is used up: everything in it must be done before it is written again
            flush(true);
            g_stage_head = 0;
            ++g_stats.wraps;
        }
        std::memcpy(g_stage_cpu + g_stage_head, p, n);
        record_copy(g_stage_gpu + g_stage_head, dst, n);
        g_stage_head = align_up(g_stage_head + n, 256);
        p += n;
        dst += n;
        bytes -= n;
    }
}

// Room in the staging ring that the GPU reads directly (vertices, per-draw uniforms): valid for the commands recorded now.
DkGpuAddr stage_alloc(const void *data, std::uint32_t bytes) {
    if (g_stage_head + bytes > kStageBytes) {
        flush(true);
        g_stage_head = 0;
        ++g_stats.wraps;
    }
    std::memcpy(g_stage_cpu + g_stage_head, data, bytes);
    const DkGpuAddr at = g_stage_gpu + g_stage_head;
    g_stage_head = align_up(g_stage_head + bytes, 256);
    return at;
}

DkMemBlock make_block(std::uint32_t bytes, std::uint32_t flags) {
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, g_dev, align_up(std::max<std::uint32_t>(bytes, 1), DK_MEMBLOCK_ALIGNMENT));
    m.flags = flags;
    return dkMemBlockCreate(&m);
}

std::uint32_t load_shader(const unsigned char *code, unsigned size, std::uint32_t &code_head) {
    std::memcpy(static_cast<std::uint8_t *>(dkMemBlockGetCpuAddr(g_code)) + code_head, code, size);
    DkShaderMaker m;
    dkShaderMakerDefaults(&m, g_code, code_head);
    DkShader s;
    dkShaderInitialize(&s, &m);
    if (!dkShaderIsValid(&s)) {
        std::fprintf(stderr, "[dk] shader %zu is not valid\n", g_shaders.size());
        return 0;
    }
    code_head = align_up(code_head + size, DK_SHADER_CODE_ALIGNMENT);
    g_shaders.push_back(s);
    return static_cast<std::uint32_t>(g_shaders.size() - 1);
}

}  // namespace

bool init_device(Limits &limits, std::string &compiler_tag) {
    compiler_tag = "deko3d";
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = on_debug;
    dm.flags = DkDeviceFlags_DepthMinusOneToOne | DkDeviceFlags_OriginLowerLeft;  // the GL conventions the shaders were written for
    step("device");
    g_dev = dkDeviceCreate(&dm);
    if (!g_dev) return false;

    step("command memory");
    g_cmdmem = make_block(kSegments * kSegmentBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, g_dev);
    cm.cbAddMem = on_cmd_out_of_memory;
    g_cmd = dkCmdBufCreate(&cm);

    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, g_dev);
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute | DkQueueFlags_HighPrio;
    qm.perWarpScratchMemorySize = 0x8000;  // the raster shader is big: room for register spills
    step("queue");
    g_queue = dkQueueCreate(&qm);
    if (!g_queue) return false;

    step("staging ring");
    g_stage = make_block(kStageBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuUncached);
    g_stage_cpu = static_cast<std::uint8_t *>(dkMemBlockGetCpuAddr(g_stage));
    g_stage_gpu = dkMemBlockGetGpuAddr(g_stage);
    open_segment();

    limits.max_storage_binding = 1ull << 30;
    limits.max_buffer = limits.max_storage_binding;
    limits.max_wg_x = 1024;
    limits.max_wg_y = 1024;
    limits.max_wg_invocations = 1024;
    limits.timestamps = false;
    std::fprintf(stderr, "[gpu] deko3d\n");
    return true;
}

void shutdown_device() {
    if (!g_dev) return;
    flush(true);
    if (g_swapchain) dkSwapchainDestroy(g_swapchain), g_swapchain = nullptr;
    for (Buf &b : g_bufs)
        if (b.mem) dkMemBlockDestroy(b.mem), b = Buf{};
    if (g_fb_mem) dkMemBlockDestroy(g_fb_mem), g_fb_mem = nullptr;
    dkQueueDestroy(g_queue);
    dkCmdBufDestroy(g_cmd);
    dkMemBlockDestroy(g_cmdmem);
    dkMemBlockDestroy(g_stage);
    if (g_code) dkMemBlockDestroy(g_code);
    dkDeviceDestroy(g_dev);
    g_dev = nullptr;
}

void pump() {}

void wait_idle() { flush(true); }

Buffer create_buffer(std::size_t bytes, std::uint32_t usage) {
    Buf b;
    b.size = align_up(static_cast<std::uint32_t>(std::max<std::size_t>(bytes, 16)), DK_UNIFORM_BUF_ALIGNMENT);
    b.cached = (usage & kMapRead) != 0;  // read back by the CPU: cached, invalidated before each read
    b.mem = make_block(b.size, (b.cached ? DkMemBlockFlags_CpuCached : DkMemBlockFlags_CpuUncached) | DkMemBlockFlags_GpuCached |
                                   DkMemBlockFlags_ZeroFillInit);
    if (!b.mem) return Buffer{};
    b.addr = dkMemBlockGetGpuAddr(b.mem);
    b.cpu = static_cast<std::uint8_t *>(dkMemBlockGetCpuAddr(b.mem));
    for (std::uint32_t id = 1; id < g_bufs.size(); ++id)
        if (!g_bufs[id].mem) {
            g_bufs[id] = b;
            return Buffer{nullptr, id};
        }
    g_bufs.push_back(b);
    return Buffer{nullptr, static_cast<std::uint32_t>(g_bufs.size() - 1)};
}

void destroy_buffer(Buffer &b) {
    if (b.id && b.id < g_bufs.size() && g_bufs[b.id].mem) {
        flush(true);  // nothing may still use it
        dkMemBlockDestroy(g_bufs[b.id].mem);
        g_bufs[b.id] = Buf{};
    }
    b = Buffer{};
}

void write_buffer(Buffer b, std::size_t offset, const void *data, std::size_t bytes) {
    const Buf &d = buf(b);
    if (!d.mem || !bytes) return;
    upload(d.addr + offset, data, bytes);
}

void write_buffer_fresh(Buffer b, const void *data, std::size_t bytes) { write_buffer(b, 0, data, bytes); }

void read_buffer(Buffer b, std::size_t offset, void *out, std::size_t bytes) {
    const Buf &d = buf(b);
    if (!d.mem || !bytes) return;
    flush(true);
    if (d.cached) dkMemBlockFlushCpuCache(d.mem, static_cast<std::uint32_t>(offset), static_cast<std::uint32_t>(bytes));
    std::memcpy(out, d.cpu + offset, bytes);
}

void copy_buffer(Buffer src, std::size_t src_off, Buffer dst, std::size_t dst_off, std::size_t bytes) {
    const Buf &s = buf(src), &d = buf(dst);
    if (!s.mem || !d.mem || !bytes) return;
    record_copy(s.addr + src_off, d.addr + dst_off, static_cast<std::uint32_t>(bytes));
}

bool map_read_async(Buffer, std::size_t, bool *busy, bool *ready) {
    flush(false);
    *busy = true;
    *ready = true;
    return true;
}

bool collect_mapped(Buffer b, void *dst, std::size_t bytes) {
    read_buffer(b, 0, dst, bytes);
    return true;
}

void set_compute_bindings(const Buffer *buffers, int count, int dynamic_slot, std::uint32_t dynamic_size) {
    g_bindings.count = std::min(count, 8);
    for (int i = 0; i < g_bindings.count; ++i) g_bindings.buffers[i] = buffers[i];
    g_bindings.dynamic_slot = dynamic_slot;
    g_bindings.dynamic_size = dynamic_size;
}

bool init_compute_shaders(int, int, bool, bool, bool, const std::string &, Pipeline *out_plain, Pipeline *out_mip) {
    struct Src { const unsigned char *code; unsigned size; };
    const Src all[] = {
#define PS2_DK_SRC(n) {kDk##n, kDk##n##_size},
        PS2_DK_SHADERS(PS2_DK_SRC)
#undef PS2_DK_SRC
    };
    std::uint32_t total = 0;
    for (const Src &s : all) total = align_up(total + s.size, DK_SHADER_CODE_ALIGNMENT);
    g_code = make_block(total + DK_SHADER_CODE_UNUSABLE_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
    if (!g_code) return false;
    step("shaders");
    std::uint32_t head = 0;
    std::uint32_t ids[sizeof all / sizeof all[0]];
    for (std::size_t i = 0; i < sizeof all / sizeof all[0]; ++i)
        if (!(ids[i] = load_shader(all[i].code, all[i].size, head))) return false;
    for (int i = 0; i < 9; ++i) out_plain[i] = Pipeline{nullptr, ids[i]};
    *out_mip = Pipeline{nullptr, ids[9]};
    g_shader_blit_vert = ids[10];
    g_shader_blit_frag = ids[11];
    g_shader_ov_vert = ids[12];
    g_shader_ov_frag = ids[13];
    g_shader_hwr_vert = ids[14];
    g_shader_hwr_frag = ids[15];
    g_shader_tex_blit_frag = ids[16];
    g_shader_texfetch = ids[17];
    if (std::getenv("PS2_HWR_SELFTEST")) hwr_self_test();
    std::fprintf(stderr, "[dk] %zu shaders, %u bytes of code\n", sizeof all / sizeof all[0], total);
    return true;
}

void destroy_pipeline(Pipeline &p) { p = Pipeline{}; }

bool attach_surface(const SurfaceTarget &, bool) {
    if (g_swapchain) return g_has_surface = true;
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, g_dev);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = kSurfaceW;
    lm.dimensions[1] = kSurfaceH;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &lm);
    const std::uint32_t one = align_up(static_cast<std::uint32_t>(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
    step("swapchain images");
    g_fb_mem = make_block(one * kSwapImages, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    if (!g_fb_mem) return false;
    DkImage const *images[kSwapImages];
    for (int i = 0; i < kSwapImages; ++i) {
        dkImageInitialize(&g_fb[i], &layout, g_fb_mem, static_cast<std::uint32_t>(i) * one);
        images[i] = &g_fb[i];
    }
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, g_dev, nwindowGetDefault(), images, kSwapImages);
    step("swapchain");
    g_swapchain = dkSwapchainCreate(&sm);
    g_has_surface = g_swapchain != nullptr && g_shader_blit_vert && g_shader_blit_frag;
    std::fprintf(stderr, "[dk] swapchain %ux%u: %s\n", kSurfaceW, kSurfaceH, g_has_surface ? "ok" : "failed");
    return g_has_surface;
}

void resize_surface(std::uint32_t, std::uint32_t) {}

bool has_surface() { return g_has_surface; }

void set_blit_buffers(Buffer scratch, Buffer blit_params) {
    g_blit_scratch = scratch;
    g_blit_ubo = blit_params;
}

void update_overlay(const void *rgba, std::uint32_t w, std::uint32_t h, std::uint32_t, std::uint32_t) {
    if (!rgba || !w || !h) return;
    const std::size_t bytes = static_cast<std::size_t>(w) * h * 4;
    if (bytes > g_overlay_cap) {
        if (g_overlay_buf) destroy_buffer(g_overlay_buf);
        g_overlay_buf = create_buffer(bytes, kStorage | kCopyDst);
        g_overlay_cap = bytes;
    }
    if (!g_overlay_rect) g_overlay_rect = create_buffer(16, kUniform | kCopyDst);
    write_buffer(g_overlay_buf, 0, rgba, bytes);
    const std::uint32_t rect[4] = {w, h, 0, 0};
    write_buffer(g_overlay_rect, 0, rect, sizeof rect);
}

namespace {

// PS2_DK_TIMING=1: a GPU timestamp after every dispatch; each submit then waits and adds the time to its shader (slow)
constexpr std::uint32_t kTsSlots = 4096;
DkMemBlock g_ts_mem{};
std::uint32_t g_ts_used = 0;
std::uint32_t g_ts_pipe[kTsSlots];

bool timing() {
    static const bool on = std::getenv("PS2_DK_TIMING") != nullptr;
    return on;
}

void timestamp(std::uint32_t pipe) {
    if (!g_ts_mem) g_ts_mem = make_block(kTsSlots * 16, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    g_ts_pipe[g_ts_used] = pipe;
    dkCmdBufReportCounter(g_cmd, DkCounter_Timestamp, dkMemBlockGetGpuAddr(g_ts_mem) + g_ts_used * 16);
    ++g_ts_used;
}

void collect_timestamps() {
    if (!g_ts_used) return;
    flush(true);
    const auto *r = static_cast<const volatile std::uint64_t *>(dkMemBlockGetCpuAddr(g_ts_mem));
    for (std::uint32_t i = 1; i < g_ts_used; ++i) {
        const std::uint32_t pipe = g_ts_pipe[i] < 32 ? g_ts_pipe[i] : 31;
        const std::uint64_t dt = r[i * 2 + 1] - r[(i - 1) * 2 + 1];
        g_stats.gpu_ms[pipe] += static_cast<double>(dt) * 625.0 / 384.0 / 1e6;  // GPU timer ticks (19.2 MHz) to ns
        ++g_stats.gpu_n[pipe];
    }
    g_ts_used = 0;
}

}  // namespace

static void run_compute_cmds(const ComputeCmd *cmds, std::size_t count) {
    if (!count) return;
    before_shader_work();
    for (int i = 0; i < g_bindings.count; ++i) {
        if (i == g_bindings.dynamic_slot) continue;
        const Buf &b = buf(g_bindings.buffers[i]);
        if (b.mem) dkCmdBufBindStorageBuffer(g_cmd, DkStage_Compute, static_cast<std::uint32_t>(i), b.addr, b.size);
    }
    const Buf &params = buf(g_bindings.buffers[g_bindings.dynamic_slot]);
    static const std::string skip = std::getenv("PS2_GL_SKIP") ? "," + std::string(std::getenv("PS2_GL_SKIP")) + "," : "";
    std::uint32_t cur = 0;
    if (timing()) timestamp(0);
    for (std::size_t i = 0; i < count; ++i) {
        const ComputeCmd &c = cmds[i];
        if (!skip.empty() && skip.find("," + std::to_string(c.pipeline.id) + ",") != std::string::npos) continue;
        if (c.pipeline.id != cur) {
            cur = c.pipeline.id;
            DkShader const *s = &g_shaders[cur];
            dkCmdBufBindShaders(g_cmd, DkStageFlag_Compute, &s, 1);
        }
        if (params.mem)
            dkCmdBufBindUniformBuffer(g_cmd, DkStage_Compute, static_cast<std::uint32_t>(g_bindings.dynamic_slot), params.addr + c.param_offset,
                                      g_bindings.dynamic_size);
        dkCmdBufDispatchCompute(g_cmd, c.gx, c.gy, c.gz);
        if (!g_stats.barriers_off) dkCmdBufBarrier(g_cmd, DkBarrier_Primitives, DkInvalidateFlags_Image);
        ++g_stats.dispatches;
        if (timing()) {
            timestamp(c.pipeline.id);
            if (g_ts_used >= kTsSlots - 1) collect_timestamps(), timestamp(0);
        }
    }
    g_after_work = true;
}

void submit_compute(const ComputeCmd *cmds, std::size_t count, bool, std::uint64_t &, std::uint64_t &) {
    run_compute_cmds(cmds, count);
    flush(false);
    if (timing()) collect_timestamps();
    g_stats.tick();
}

static void draw_fullscreen(std::uint32_t vert, std::uint32_t frag, Buffer storage, Buffer ubo) {
    DkShader const *shaders[2] = {&g_shaders[vert], &g_shaders[frag]};
    dkCmdBufBindShaders(g_cmd, DkStageFlag_GraphicsMask, shaders, 2);
    const Buf &s = buf(storage), &u = buf(ubo);
    if (s.mem) dkCmdBufBindStorageBuffer(g_cmd, DkStage_Fragment, 0, s.addr, s.size);
    if (u.mem) dkCmdBufBindUniformBuffer(g_cmd, DkStage_Fragment, 1, u.addr, u.size);
    dkCmdBufDraw(g_cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
}

bool submit_present(const ComputeCmd *cmds, std::size_t count, const PresentInfo &info, bool, std::uint64_t &, std::uint64_t &) {
    run_compute_cmds(cmds, count);
    if (!g_has_surface) {
        flush(false);
        return false;
    }
    flush(false);  // the compute work goes out before the queue waits for a free picture
    if (timing()) collect_timestamps();
    const auto acquire0 = std::chrono::steady_clock::now();
    const int slot = dkQueueAcquireImage(g_queue, g_swapchain);
    g_stats.blocked_ms += ms_since(acquire0);
    before_shader_work();
    DkImageView view;
    dkImageViewDefaults(&view, &g_fb[slot]);
    DkImageView const *target = &view;
    dkCmdBufBindRenderTargets(g_cmd, &target, 1, nullptr);
    const DkScissor all{0, 0, kSurfaceW, kSurfaceH};
    dkCmdBufSetScissors(g_cmd, 0, &all, 1);
    const DkViewport full{0.0f, 0.0f, static_cast<float>(kSurfaceW), static_cast<float>(kSurfaceH), 0.0f, 1.0f};
    dkCmdBufSetViewports(g_cmd, 0, &full, 1);
    dkCmdBufClearColorFloat(g_cmd, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;
    dkCmdBufBindRasterizerState(g_cmd, &rs);
    DkColorState cs;
    dkColorStateDefaults(&cs);
    dkCmdBufBindColorState(g_cmd, &cs);
    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    dkCmdBufBindColorWriteState(g_cmd, &cw);
    dkCmdBufBindVtxAttribState(g_cmd, nullptr, 0);
    dkCmdBufBindVtxBufferState(g_cmd, nullptr, 0);
    // the surface is fixed at 1280x720; gpu.cpp places the picture for the size it was told (attach_surface)
    const DkViewport vp{info.vp_x, info.vp_y, info.vp_w, info.vp_h, 0.0f, 1.0f};
    dkCmdBufSetViewports(g_cmd, 0, &vp, 1);
    draw_fullscreen(g_shader_blit_vert, g_shader_blit_frag, g_blit_scratch, g_blit_ubo);
    if (info.overlay_on && g_overlay_buf && g_shader_ov_vert && g_shader_ov_frag) {
        dkColorStateSetBlendEnable(&cs, 0, true);
        dkCmdBufBindColorState(g_cmd, &cs);
        DkBlendState bs;
        dkBlendStateDefaults(&bs);
        dkBlendStateSetFactors(&bs, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha, DkBlendFactor_One, DkBlendFactor_InvSrcAlpha);
        dkCmdBufBindBlendState(g_cmd, 0, &bs);
        const DkViewport ov{static_cast<float>(info.ov_x), static_cast<float>(info.ov_y), static_cast<float>(info.ov_w), static_cast<float>(info.ov_h), 0.0f, 1.0f};
        dkCmdBufSetViewports(g_cmd, 0, &ov, 1);
        draw_fullscreen(g_shader_ov_vert, g_shader_ov_frag, g_overlay_buf, g_overlay_rect);
    }
    dkCmdBufBarrier(g_cmd, DkBarrier_Fragments, 0);
    g_after_work = true;
    flush(false);
    dkQueuePresentImage(g_queue, g_swapchain, slot);
    ++g_stats.presents;
    g_stats.tick();
    return true;
}


// ---------------------------------------------------------------- GPU triangle renderer API (backend.hpp)

namespace {

constexpr std::uint32_t kMaxImages = 1024;  // descriptor slots; slot = Image id
constexpr std::uint32_t kSamplers = 4;      // bit 0 linear, bit 1 clamp to edge

struct Img {
    DkMemBlock mem{};
    DkImage image{};
    std::uint32_t w = 0, h = 0;
    bool rt = false, depth = false;
};
std::vector<Img> g_images(1);
DkMemBlock g_desc{};  // image descriptors [kMaxImages], then sampler descriptors [kSamplers]
bool g_desc_dirty = true;

DkImageDescriptor *image_descs() { return static_cast<DkImageDescriptor *>(dkMemBlockGetCpuAddr(g_desc)); }

bool ensure_descriptors() {
    if (g_desc) return true;
    g_desc = make_block((kMaxImages + kSamplers) * 32, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    if (!g_desc) return false;
    auto *samplers = reinterpret_cast<DkSamplerDescriptor *>(image_descs() + kMaxImages);
    for (std::uint32_t i = 0; i < kSamplers; ++i) {
        DkSampler s;
        dkSamplerDefaults(&s);
        s.minFilter = s.magFilter = (i & 1) ? DkFilter_Linear : DkFilter_Nearest;
        s.wrapMode[0] = s.wrapMode[1] = (i & 2) ? DkWrapMode_ClampToEdge : DkWrapMode_Repeat;
        dkSamplerDescriptorInitialize(&samplers[i], &s);
    }
    return true;
}

// descriptors written by the CPU are used by the GPU only after the descriptor cache is invalidated
void bind_descriptors() {
    if (g_desc_dirty) {
        dkCmdBufBarrier(g_cmd, DkBarrier_Full, DkInvalidateFlags_Descriptors);
        g_desc_dirty = false;
    }
    const DkGpuAddr base = dkMemBlockGetGpuAddr(g_desc);
    dkCmdBufBindImageDescriptorSet(g_cmd, base, kMaxImages);
    dkCmdBufBindSamplerDescriptorSet(g_cmd, base + kMaxImages * 32, kSamplers);
}

const Img *img(Image i) { return i.id && i.id < g_images.size() && g_images[i.id].mem ? &g_images[i.id] : nullptr; }

void bind_target(const Img &t, const Img *depth = nullptr) {
    DkImageView view, dview;
    dkImageViewDefaults(&view, &t.image);
    DkImageView const *target = &view;
    if (depth) dkImageViewDefaults(&dview, &depth->image);
    dkCmdBufBindRenderTargets(g_cmd, &target, 1, depth ? &dview : nullptr);
    const DkViewport vp{0.0f, 0.0f, static_cast<float>(t.w), static_cast<float>(t.h), 0.0f, 1.0f};
    dkCmdBufSetViewports(g_cmd, 0, &vp, 1);
}

void plain_raster_state(bool blend) {
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;
    dkCmdBufBindRasterizerState(g_cmd, &rs);
    DkColorState cs;
    dkColorStateDefaults(&cs);
    dkColorStateSetBlendEnable(&cs, 0, blend);
    dkCmdBufBindColorState(g_cmd, &cs);
    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    dkCmdBufBindColorWriteState(g_cmd, &cw);
}

// PS2_DK_SCREEN_DUMP=<file>@<picture>: the swapchain picture as a PPM (rows as the GPU stores them), to check what reaches the screen
void screen_dump(int slot) {
    static const char *spec = std::getenv("PS2_DK_SCREEN_DUMP");
    static std::uint64_t pictures = 0;
    if (!spec) return;
    // <file>@<n>[,<n>...]: a %u in the file name becomes the picture number
    const char *at = std::strrchr(spec, '@');
    if (!at) return;
    ++pictures;
    bool hit = false;
    for (const char *p = at + 1; *p;) {
        char *end = nullptr;
        const unsigned long long n = std::strtoull(p, &end, 10);
        if (end == p) break;
        hit |= n == pictures;
        if (*end != ',') break;
        p = end + 1;
    }
    if (!hit) return;
    char name[256];
    std::snprintf(name, sizeof name, std::string(spec, at).c_str(), static_cast<unsigned>(pictures));
    const std::uint32_t w = kSurfaceW, h = kSurfaceH;
    Buffer rb = create_buffer(static_cast<std::size_t>(w) * h * 4, kMapRead | kCopyDst);
    dkCmdBufBarrier(g_cmd, DkBarrier_Full, 0);
    DkImageView view;
    dkImageViewDefaults(&view, &g_fb[slot]);
    const DkImageRect rect{0, 0, 0, w, h, 1};
    const DkCopyBuf to{buf(rb).addr, 0, 0};
    dkCmdBufCopyImageToBuffer(g_cmd, &view, &rect, &to, 0);
    g_recorded = true;
    std::vector<std::uint32_t> px(static_cast<std::size_t>(w) * h);
    read_buffer(rb, 0, px.data(), px.size() * 4);
    destroy_buffer(rb);
    if (std::FILE *f = std::fopen(name, "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (std::uint32_t y = h; y-- > 0;)  // rows reversed: the dump then shows what the screen shows
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::uint32_t p = px[static_cast<std::size_t>(y) * w + x];
                std::fputc(p & 0xFF, f), std::fputc((p >> 8) & 0xFF, f), std::fputc((p >> 16) & 0xFF, f);
            }
        std::fclose(f);
        std::fprintf(stderr, "[dk] screen of picture %llu written\n", static_cast<unsigned long long>(pictures));
    }
}

}  // namespace

namespace {
Image make_image(std::uint32_t w, std::uint32_t h, bool render_target, bool depth) {
    if (!w || !h || !ensure_descriptors()) return Image{};
    std::uint32_t id = 1;
    while (id < g_images.size() && g_images[id].mem) ++id;
    if (id >= kMaxImages) return Image{};
    if (id == g_images.size()) g_images.emplace_back();
    Img &m = g_images[id];
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, g_dev);
    lm.flags = (render_target || depth ? DkImageFlags_UsageRender : 0u) | (depth ? 0u : DkImageFlags_Usage2DEngine);
    lm.format = depth ? DkImageFormat_ZF32 : DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = w;
    lm.dimensions[1] = h;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &lm);
    m.mem = make_block(static_cast<std::uint32_t>(dkImageLayoutGetSize(&layout)), DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    if (!m.mem) return Image{};
    dkImageInitialize(&m.image, &layout, m.mem, 0);
    m.w = w, m.h = h, m.rt = render_target, m.depth = depth;
    if (!depth) {
        DkImageView view;
        dkImageViewDefaults(&view, &m.image);
        dkImageDescriptorInitialize(&image_descs()[id], &view, false, false);
        g_desc_dirty = true;
    }
    return Image{id};
}
}  // namespace

Image create_image(std::uint32_t w, std::uint32_t h, bool render_target) { return make_image(w, h, render_target, false); }

Image create_depth_image(std::uint32_t w, std::uint32_t h) { return make_image(w, h, false, true); }

void copy_image(Image src, Image dst) {
    const Img *s = img(src), *d = img(dst);
    if (!s || !d || s->depth || d->depth) return;
    if (g_after_work) {
        dkCmdBufBarrier(g_cmd, DkBarrier_Full, 0);
        g_after_work = false;
    }
    DkImageView sv, dv;
    dkImageViewDefaults(&sv, &s->image);
    dkImageViewDefaults(&dv, &d->image);
    const DkImageRect rect{0, 0, 0, std::min(s->w, d->w), std::min(s->h, d->h), 1};
    dkCmdBufCopyImage(g_cmd, &sv, &rect, &dv, &rect, 0);
    g_after_copy = true;
    g_recorded = true;
}

void clear_depth(Image target, Image depth, float value) {
    const Img *t = img(target), *d = img(depth);
    if (!t || !t->rt || !d || !d->depth) return;
    before_shader_work();
    bind_target(*t, d);
    const DkScissor all{0, 0, t->w, t->h};
    dkCmdBufSetScissors(g_cmd, 0, &all, 1);
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthWriteEnable = true;
    dkCmdBufBindDepthStencilState(g_cmd, &ds);
    dkCmdBufClearDepthStencil(g_cmd, true, value, 0, 0);
    g_after_work = true;
}

void destroy_image(Image &i) {
    if (img(i)) {
        flush(true);  // nothing may still use it
        dkMemBlockDestroy(g_images[i.id].mem);
        g_images[i.id] = Img{};
    }
    i = Image{};
}

void clear_image(Image i, float r, float g, float b, float a) {
    const Img *t = img(i);
    if (!t || !t->rt) return;
    before_shader_work();
    bind_target(*t);
    const DkScissor all{0, 0, t->w, t->h};
    dkCmdBufSetScissors(g_cmd, 0, &all, 1);
    dkCmdBufClearColorFloat(g_cmd, 0, DkColorMask_RGBA, r, g, b, a);
    g_after_work = true;
}

void upload_image_from_buffer(Buffer src, std::size_t offset, Image dst, std::uint32_t w, std::uint32_t h) {
    const Buf &s = buf(src);
    const Img *t = img(dst);
    if (!s.mem || !t) return;
    if (g_after_work) {
        dkCmdBufBarrier(g_cmd, DkBarrier_Full, 0);
        g_after_work = false;
    }
    DkImageView view;
    dkImageViewDefaults(&view, &t->image);
    const DkCopyBuf from{s.addr + offset, 0, 0};
    const DkImageRect rect{0, 0, 0, std::min(w, t->w), std::min(h, t->h), 1};
    dkCmdBufCopyBufferToImage(g_cmd, &from, &view, &rect, 0);
    g_after_copy = true;
    g_recorded = true;
}

bool hwr_draw(const HwrVertex *verts, std::uint32_t nverts, const HwrDraw *draws, std::uint32_t ndraws) {
    if (!nverts || !ndraws || !g_shader_hwr_vert || !g_shader_hwr_frag || !ensure_descriptors()) return false;
    before_shader_work();
    ++g_stats.hwr_batches;
    if (timing()) {  // PS2_DK_TIMING: this batch's GPU time is reported as shader 30
        if (g_ts_used >= kTsSlots - 2) collect_timestamps();
        timestamp(0);
    }
    const DkGpuAddr vb = stage_alloc(verts, nverts * sizeof(HwrVertex));
    DkShader const *shaders[2] = {&g_shaders[g_shader_hwr_vert], &g_shaders[g_shader_hwr_frag]};
    dkCmdBufBindShaders(g_cmd, DkStageFlag_GraphicsMask, shaders, 2);
    static const DkVtxAttribState attribs[3] = {
        {0, 0, offsetof(HwrVertex, x), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(HwrVertex, rgba), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
        {0, 0, offsetof(HwrVertex, s), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
    };
    static const DkVtxBufferState vbs{sizeof(HwrVertex), 0};
    dkCmdBufBindVtxAttribState(g_cmd, attribs, 3);
    dkCmdBufBindVtxBufferState(g_cmd, &vbs, 1);
    dkCmdBufBindVtxBuffer(g_cmd, 0, vb, nverts * sizeof(HwrVertex));
    plain_raster_state(true);
    bind_descriptors();
    std::uint32_t bound_target = 0, bound_depth = ~0u;
    int bound_blend = -1, bound_z = -1, bound_mask = -1;
    for (std::uint32_t i = 0; i < ndraws; ++i) {
        const HwrDraw &d = draws[i];
        const Img *t = img(d.target);
        if (!t || !t->rt || d.first + d.count > nverts) continue;
        const Img *z = img(d.depth);
        if (z && (!z->depth || z->w != t->w || z->h != t->h)) z = nullptr;
        if (d.target.id != bound_target || (z ? d.depth.id : 0u) != bound_depth) {
            bind_target(*t, z);
            bound_target = d.target.id, bound_depth = z ? d.depth.id : 0u;
        }
        // dual-source blending: output 1 alpha is the GS blend factor (dk/hwr.frag); hwr::Blend rows
        if (d.blend != bound_blend) {
            DkBlendState bs;
            dkBlendStateDefaults(&bs);
            // hwr::Blend: 1 add, 2 reverse subtract, 3 source only, 5 Cd*f + Cs, 6 (Cd - Cs)*f, 7 Cd*f + Cd; 0 and 4 lerp
            const bool add = d.blend == 1, rev = d.blend == 2 || d.blend == 6, src_only = d.blend == 3, dst_scale = d.blend == 5;
            const bool brighten = d.blend == 7;  // the shader outputs f: f * Cd + Cd
            // RGB: the GS factor per channel (output 1 rgb); alpha is never blended on the GS: the source alpha is written
            // (output 1 alpha = 1) or the frame's kept (0)
            const DkBlendFactor src = dst_scale ? DkBlendFactor_One : brighten ? DkBlendFactor_DstColor : DkBlendFactor_Src1Color;
            const DkBlendFactor dst = add || d.blend == 2 || brighten ? DkBlendFactor_One : src_only ? DkBlendFactor_Zero
                                    : dst_scale || d.blend == 6 ? DkBlendFactor_Src1Color : DkBlendFactor_InvSrc1Color;
            dkBlendStateSetOps(&bs, rev ? DkBlendOp_RevSub : DkBlendOp_Add, DkBlendOp_Add);
            dkBlendStateSetFactors(&bs, src, dst, DkBlendFactor_Src1Alpha, DkBlendFactor_InvSrc1Alpha);
            dkCmdBufBindBlendState(g_cmd, 0, &bs);
            bound_blend = d.blend;
        }
        if (d.write_mask != bound_mask) {  // FBMSK in whole bytes
            DkColorWriteState cw;
            dkColorWriteStateDefaults(&cw);
            dkColorWriteStateSetMask(&cw, 0, d.write_mask);
            dkCmdBufBindColorWriteState(g_cmd, &cw);
            bound_mask = d.write_mask;
        }
        const int zkey = z ? (d.ztest | (d.zwrite ? 8 : 0)) : 0;
        if (zkey != bound_z) {
            DkDepthStencilState ds;
            dkDepthStencilStateDefaults(&ds);
            ds.depthTestEnable = z && d.ztest != 0;
            ds.depthWriteEnable = z && d.zwrite;
            ds.depthCompareOp = d.ztest == 1 ? DkCompareOp_Never : d.ztest == 3 ? DkCompareOp_Gequal : d.ztest == 4 ? DkCompareOp_Greater : DkCompareOp_Always;
            dkCmdBufBindDepthStencilState(g_cmd, &ds);
            bound_z = zkey;
        }
        // scissor rows count from the top in GS terms; the device uses the GL convention (rows from the bottom)
        const std::uint32_t x0 = std::min(d.scissor[0], t->w), x1 = std::min(d.scissor[2] + 1, t->w);
        const std::uint32_t y0 = std::min(d.scissor[1], t->h), y1 = std::min(d.scissor[3] + 1, t->h);
        if (x1 <= x0 || y1 <= y0) continue;
        const DkScissor sc{x0, t->h - y1, x1 - x0, y1 - y0};
        dkCmdBufSetScissors(g_cmd, 0, &sc, 1);
        struct {
            std::uint32_t state[16];
            float size[4];
        } ubo{};
        std::memcpy(ubo.state, d.state, sizeof ubo.state);
        ubo.size[0] = static_cast<float>(t->w), ubo.size[1] = static_cast<float>(t->h);
        const DkGpuAddr u = stage_alloc(&ubo, sizeof ubo);
        dkCmdBufBindUniformBuffer(g_cmd, DkStage_Vertex, 0, u, sizeof ubo);
        dkCmdBufBindUniformBuffer(g_cmd, DkStage_Fragment, 0, u, sizeof ubo);
        if (img(d.texture)) {
            const std::uint32_t sampler = ((d.state[0] >> 2) & 1u) | (((d.state[0] >> 3) & 1u) << 1);
            dkCmdBufBindTexture(g_cmd, DkStage_Fragment, 0, dkMakeTextureHandle(d.texture.id, sampler));
        }
        dkCmdBufDraw(g_cmd, DkPrimitive_Triangles, d.count, 1, d.first, 0);
        g_recorded = true;  // a ring wrap inside this loop opened a new segment: these draws belong to it
        ++g_stats.hwr_draws;
    }
    dkCmdBufBarrier(g_cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
    if (timing()) timestamp(30);
    g_after_work = true;
    return true;
}

bool present_image(Image src, const PresentInfo &info, std::uint32_t region_x, std::uint32_t region_y, std::uint32_t region_w, std::uint32_t region_h) {
    const Img *s = img(src);
    if (!s || !g_has_surface || !g_shader_ov_vert || !g_shader_tex_blit_frag) return false;
    flush(false);
    const int slot = dkQueueAcquireImage(g_queue, g_swapchain);
    before_shader_work();
    DkImageView view;
    dkImageViewDefaults(&view, &g_fb[slot]);
    DkImageView const *target = &view;
    dkCmdBufBindRenderTargets(g_cmd, &target, 1, nullptr);
    const DkScissor all{0, 0, kSurfaceW, kSurfaceH};
    dkCmdBufSetScissors(g_cmd, 0, &all, 1);
    const DkViewport full{0.0f, 0.0f, static_cast<float>(kSurfaceW), static_cast<float>(kSurfaceH), 0.0f, 1.0f};
    dkCmdBufSetViewports(g_cmd, 0, &full, 1);
    dkCmdBufClearColorFloat(g_cmd, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
    plain_raster_state(false);
    dkCmdBufBindVtxAttribState(g_cmd, nullptr, 0);
    dkCmdBufBindVtxBufferState(g_cmd, nullptr, 0);
    bind_descriptors();
    const DkViewport vp{info.vp_x, info.vp_y, info.vp_w, info.vp_h, 0.0f, 1.0f};
    dkCmdBufSetViewports(g_cmd, 0, &vp, 1);
    DkShader const *shaders[2] = {&g_shaders[g_shader_ov_vert], &g_shaders[g_shader_tex_blit_frag]};
    dkCmdBufBindShaders(g_cmd, DkStageFlag_GraphicsMask, shaders, 2);
    dkCmdBufBindTexture(g_cmd, DkStage_Fragment, 0, dkMakeTextureHandle(src.id, 1 | 2));
    const float region[4] = {static_cast<float>(region_w) / static_cast<float>(s->w), static_cast<float>(region_h) / static_cast<float>(s->h),
                             // rows: the device uses the GL convention (memory row 0 at the bottom), so GS row r is at row h-1-r
                             static_cast<float>(region_x) / static_cast<float>(s->w),
                             static_cast<float>(s->h - std::min(s->h, region_y + region_h)) / static_cast<float>(s->h)};
    const DkGpuAddr u = stage_alloc(region, sizeof region);
    dkCmdBufBindUniformBuffer(g_cmd, DkStage_Fragment, 0, u, sizeof region);
    dkCmdBufDraw(g_cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
    dkCmdBufBarrier(g_cmd, DkBarrier_Fragments, 0);
    g_after_work = true;
    screen_dump(slot);
    flush(false);
    dkQueuePresentImage(g_queue, g_swapchain, slot);
    ++g_stats.presents;
    g_stats.tick();
    return true;
}

bool read_image(Image src, std::uint32_t *rgba, std::uint32_t w, std::uint32_t h) {
    const Img *s = img(src);
    if (!s || !rgba) return false;
    w = std::min(w, s->w), h = std::min(h, s->h);
    Buffer rb = create_buffer(static_cast<std::size_t>(w) * h * 4, kMapRead | kCopyDst);
    if (g_after_work) {
        dkCmdBufBarrier(g_cmd, DkBarrier_Full, 0);
        g_after_work = false;
    }
    DkImageView view;
    dkImageViewDefaults(&view, &s->image);
    const DkImageRect rect{0, 0, 0, w, h, 1};
    const DkCopyBuf to{buf(rb).addr, 0, 0};
    dkCmdBufCopyImageToBuffer(g_cmd, &view, &rect, &to, 0);
    g_recorded = true;
    read_buffer(rb, 0, rgba, static_cast<std::size_t>(w) * h * 4);
    destroy_buffer(rb);
    return true;
}

Pipeline texfetch_pipeline() { return Pipeline{nullptr, g_shader_texfetch}; }

void hwr_self_test() {
    constexpr std::uint32_t n = 64;
    Image t = create_image(n, n, true);
    if (!t) {
        std::fprintf(stderr, "[hwr] self-test: FAILED (no image)\n");
        return;
    }
    clear_image(t, 0.0f, 0.0f, 0.0f, 1.0f);
    // mode (PS2_HWR_SELFTEST): 1 plain red left half; 16 / 32 the shader's debug colours; 2 blending + depth
    const int mode = std::atoi(std::getenv("PS2_HWR_SELFTEST"));
    constexpr std::uint32_t kNoBlend = 1u << 15;
    const float one = 1.0f;
    std::uint32_t zscale_one;  // GS Z used as 0..1 depth directly
    std::memcpy(&zscale_one, &one, 4);
    const std::uint32_t red = 0xFF0000FFu;  // RGBA8 little-endian: r in the low byte
    const auto rect = [](HwrVertex *o, float x0, float x1, float z, std::uint32_t rgba) {
        const HwrVertex a{x0, 0, z, 0, rgba, 0, 0, 1}, b{x1, 0, z, 0, rgba, 0, 0, 1}, c{x1, 64, z, 0, rgba, 0, 0, 1}, e{x0, 64, z, 0, rgba, 0, 0, 1};
        o[0] = a, o[1] = b, o[2] = c, o[3] = a, o[4] = c, o[5] = e;
    };
    HwrVertex v[12];
    HwrDraw d[2]{};
    for (HwrDraw &x : d) x.target = t, x.scissor[2] = x.scissor[3] = n - 1, x.count = 6, x.state[4] = zscale_one;
    std::uint32_t want_left = red, want_right = 0xFF000000u;
    Image depth{};
    if (mode == 2) {
        // A: red, closer (z 0.75), left half, written with depth; B: green with source alpha 64 (factor 0.5, lerp), farther (z 0.5),
        // everywhere, depth test GREATER: rejected over A, blended 50 % over black elsewhere
        depth = create_depth_image(n, n);
        clear_depth(t, depth, 0.0f);
        rect(v, 0, 32, 0.75f, red);
        rect(v + 6, 0, 64, 0.5f, 0x4000FF00u);
        d[0].depth = d[1].depth = depth;
        d[0].state[0] = kNoBlend, d[0].ztest = 2, d[0].zwrite = true;
        d[1].first = 6, d[1].blend = 0, d[1].ztest = 4, d[1].zwrite = true;
        want_right = 0;  // checked by range below
        hwr_draw(v, 12, d, 2);
    } else {
        rect(v, 0, 32, 0.0f, red);
        d[0].state[0] = kNoBlend | (mode == 16 ? 1u << 16 : 0u) | (mode == 32 ? 1u << 17 : 0u);
        hwr_draw(v, 6, d, 1);
    }
    std::vector<std::uint32_t> px(n * n);
    read_image(t, px.data(), n, n);
    const std::uint32_t left = px[10 * n + 10], right = px[10 * n + 50];
    const std::uint32_t rg = (right >> 8) & 0xFF;
    const bool ok = mode == 2 ? left == red && (right & 0xFF) == 0 && rg >= 126 && rg <= 129 : left == want_left && right == want_right;
    if (ok) std::fprintf(stderr, "[hwr] self-test %d: ok\n", mode);
    else std::fprintf(stderr, "[hwr] self-test %d: FAILED (pixel (10,10) = 0x%08X, (50,10) = 0x%08X)\n", mode, left, right);
    if (depth) destroy_image(depth);
    destroy_image(t);
}

}  // namespace ps2::gs::gpu::be
