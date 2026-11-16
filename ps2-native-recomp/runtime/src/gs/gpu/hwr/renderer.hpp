#pragma once
// The GPU triangle renderer (docs/superpowers/specs/2026-11-15-switch-hw-renderer-design.md): deko3d, and the PC OpenGL build.
// Stage 1: GS triangles/sprites drawn by deko3d into GPU images (one per frame buffer), textured from a cache filled by the
// painter's own texel fetch (scripts/glsl_raster_lean.py --texfetch); no blending, depth or alpha test yet; shapes that
// need the painter are left out. PS2_HWR=1 turns it on.

#include "../backend.hpp"

#include <cstddef>
#include <cstdint>

namespace ps2::gs::hwr {

bool active();
// one shape from gpu::draw; `setup` = the painter's setup words for it (kSetupWords), used to fetch its texture
void draw(const ps2::gs::State &s, const ps2::gs::Vertex *v, int count, const std::uint32_t *setup);
// a GS-to-GS copy wrote this rectangle of the buffer at DBP (64-word blocks), DBW x 64 pixels wide: its pages are new content
void uploaded(std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dpsm, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h);
// an EE upload into that rectangle starts; when all of it arrived, upload_done with the hash of its data (ContentHash). Textures
// over pages that end up with the same uploads as before (the same data into the same places) are not fetched again.
void upload_begin(std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dpsm, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h);
void hashed(double ms);  // time gpu.cpp spent hashing upload data (PS2_HWR_LOG)
// a palette load (TEX0 with CLD) from GS memory into the palette buffer (gpu::load_clut)
void clut_loaded(std::uint64_t tex0, std::uint64_t texclut);
void upload_done(std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dpsm, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h,
                 std::uint64_t hash);

// 64-bit hash of a byte stream that arrives in pieces (four XXH64-style lanes; the same result however the stream is split)
struct ContentHash {
    std::uint64_t lane[4] = {0x9E3779B185EBCA87ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x85EBCA77C2B2AE63ull};
    std::uint8_t tail[32];
    std::uint32_t tail_n = 0;
    std::uint64_t bytes = 0;
    static std::uint64_t round(std::uint64_t acc, std::uint64_t v) {
        acc += v * 0xC2B2AE3D27D4EB4Full;
        acc = (acc << 31) | (acc >> 33);
        return acc * 0x9E3779B185EBCA87ull;
    }
    void block(const std::uint8_t *p) {
        for (int i = 0; i < 4; ++i) {
            std::uint64_t v;
            __builtin_memcpy(&v, p + i * 8, 8);
            lane[i] = round(lane[i], v);
        }
    }
    void add(const std::uint8_t *p, std::size_t n) {
        bytes += n;
        if (tail_n) {
            const std::size_t take = n < 32 - tail_n ? n : 32 - tail_n;
            __builtin_memcpy(tail + tail_n, p, take);
            tail_n += static_cast<std::uint32_t>(take), p += take, n -= take;
            if (tail_n < 32) return;
            block(tail);
            tail_n = 0;
        }
        for (; n >= 32; p += 32, n -= 32) block(p);
        __builtin_memcpy(tail, p, n);
        tail_n = static_cast<std::uint32_t>(n);
    }
    std::uint64_t done() const {
        std::uint64_t h = bytes;
        for (std::uint64_t l : lane) h = round(h ^ l, l);
        for (std::uint32_t i = 0; i < tail_n; ++i) h = round(h, tail[i] | (static_cast<std::uint64_t>(i) << 8));
        return h ^ (h >> 29);
    }
};
bool present(const ps2::gs::gpu::DisplaySource &src, std::uint32_t surface_w, std::uint32_t surface_h, bool widescreen);

}  // namespace ps2::gs::hwr

namespace ps2::gs::gpu {
// in gpu.cpp (needs the painter's buffers): runs the texture fetch for `setup` into `dst` (w x h texels)
bool hwr_fetch_texture(const std::uint32_t *setup, std::uint32_t w, std::uint32_t h, be::Image dst);
void hwr_flush_fetches();  // runs the queued fetches; before draws that use their images
}  // namespace ps2::gs::gpu
