#pragma once

#include "gpu.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace ps2::gs::gpu::be {

enum BufferUsage : std::uint32_t {
    kStorage = 1u << 0,
    kUniform = 1u << 1,
    kCopySrc = 1u << 2,
    kCopyDst = 1u << 3,
    kMapRead = 1u << 4,
};

struct Buffer {
    void *ptr = nullptr;
    std::uint32_t id = 0;
    explicit operator bool() const { return ptr != nullptr || id != 0; }
};

struct Pipeline {
    void *ptr = nullptr;
    std::uint32_t id = 0;
    explicit operator bool() const { return ptr != nullptr || id != 0; }
};

struct Limits {
    std::uint64_t max_storage_binding = 0;
    std::uint64_t max_buffer = 0;
    std::uint32_t max_wg_x = 256;
    std::uint32_t max_wg_y = 256;
    std::uint32_t max_wg_invocations = 256;
    bool timestamps = false;
};

struct ComputeCmd {
    Pipeline pipeline;
    std::uint32_t param_offset = 0;
    std::uint32_t gx = 1, gy = 1, gz = 1;
};

struct PresentInfo {
    std::uint32_t surface_w = 0, surface_h = 0;
    float vp_x = 0.0f, vp_y = 0.0f, vp_w = 0.0f, vp_h = 0.0f;
    bool fsr = false;
    bool overlay_on = false;
    std::uint32_t ov_x = 0, ov_y = 0, ov_w = 0, ov_h = 0;
};

bool init_device(Limits &limits, std::string &compiler_tag);
void shutdown_device();
void pump();
void wait_idle();

Buffer create_buffer(std::size_t bytes, std::uint32_t usage);
void destroy_buffer(Buffer &b);
void write_buffer(Buffer b, std::size_t offset, const void *data, std::size_t bytes);
// The whole content is replaced (per-submit data): the old storage may still be in use by the GPU, so take new storage
// instead of waiting for it (orphaning)
void write_buffer_fresh(Buffer b, const void *data, std::size_t bytes);
void read_buffer(Buffer b, std::size_t offset, void *out, std::size_t bytes);
void copy_buffer(Buffer src, std::size_t src_off, Buffer dst, std::size_t dst_off, std::size_t bytes);
bool map_read_async(Buffer b, std::size_t bytes, bool *busy, bool *ready);
bool collect_mapped(Buffer b, void *dst, std::size_t bytes);

void set_compute_bindings(const Buffer *buffers, int count, int dynamic_slot, std::uint32_t dynamic_size);
bool init_compute_shaders(int scale, int tile, bool explicit_fma, bool tex_ss, bool depth_frac,
                          const std::string &override_src, Pipeline *out_plain, Pipeline *out_mip);
void destroy_pipeline(Pipeline &p);

bool attach_surface(const SurfaceTarget &target, bool fsr);
void resize_surface(std::uint32_t w, std::uint32_t h);
bool has_surface();
void set_blit_buffers(Buffer scratch, Buffer blit_params);
void update_overlay(const void *rgba, std::uint32_t w, std::uint32_t h, std::uint32_t x, std::uint32_t y);

// ---- GPU triangle renderer (docs/superpowers/specs/2026-11-15-switch-hw-renderer-design.md); deko3d and OpenGL (PC), other backends
// return false / an empty Image
struct Image {
    std::uint32_t id = 0;
    explicit operator bool() const { return id != 0; }
};
struct HwrVertex {  // screen pixels (x, y), z, w; RGBA8 colour; texture s, t, q
    float x, y, z, w;
    std::uint32_t rgba;
    float s, t, q;
};
struct HwrDraw {
    Image target, texture, depth;  // depth: optional, same size as target
    std::uint32_t scissor[4];  // x0, y0, x1, y1 inclusive, in target pixels
    std::uint32_t first, count;
    std::uint32_t state[16];   // the uber shader's uniforms (dk/hwr.frag; hwr/state.hpp GpuState::shader in state[0..7])
    std::uint8_t blend = 0;    // hwr::Blend: 0 lerp, 1 add, 2 reverse subtract, 3 source only, 4 unmapped (as lerp)
    std::uint8_t ztest = 0;    // hwr::ZTest: 0 off, 1 never, 2 always, 3 gequal, 4 greater
    bool zwrite = false;
    std::uint8_t write_mask = 0xF;  // R 1, G 2, B 4, A 8
};
Image create_image(std::uint32_t w, std::uint32_t h, bool render_target);
Image create_depth_image(std::uint32_t w, std::uint32_t h);  // 32-bit float depth, for HwrDraw::depth
void clear_depth(Image target, Image depth, float value);
void copy_image(Image src, Image dst);  // same size; ordered in the command stream
void destroy_image(Image &img);
void clear_image(Image img, float r, float g, float b, float a);
void upload_image_from_buffer(Buffer src, std::size_t offset, Image dst, std::uint32_t w, std::uint32_t h);
bool hwr_draw(const HwrVertex *verts, std::uint32_t nverts, const HwrDraw *draws, std::uint32_t ndraws);
bool present_image(Image src, const PresentInfo &info, std::uint32_t region_x, std::uint32_t region_y, std::uint32_t region_w, std::uint32_t region_h);  // shows that region of the image
bool read_image(Image src, std::uint32_t *rgba, std::uint32_t w, std::uint32_t h);
void hwr_self_test();  // PS2_HWR_SELFTEST=1: one triangle into a 64x64 image, read back, "[hwr] self-test: ok" / "FAILED"
Pipeline texfetch_pipeline();  // compute: prm(0) setup words, prm(1) w, prm(2) h, prm(3) first output word (binding 7)

void submit_compute(const ComputeCmd *cmds, std::size_t count, bool measure_ts, std::uint64_t &ns_accum, std::uint64_t &ns_count);
bool submit_present(const ComputeCmd *cmds, std::size_t count, const PresentInfo &info,
                    bool measure_ts, std::uint64_t &ns_accum, std::uint64_t &ns_count);

}  // namespace ps2::gs::gpu::be
