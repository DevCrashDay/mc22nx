#pragma once
// Which 2048-word GS pages a rectangle of a buffer covers (for the GPU renderer's texture invalidation and target
// tracking). Page sizes by pixel format per the GS manual: 32-bit 64x32, 16-bit 64x64, 8-bit 128x64, 4-bit 128x128
// (T8H/T4HL/T4HH live in 32-bit pages). Pure; tests/hwr_pages_test.cpp checks it.

#include <algorithm>
#include <cstdint>

namespace ps2::gs::hwr {

struct PageSize {
    std::uint32_t w, h;
};

inline PageSize page_size(std::uint32_t psm) {
    if (psm == 0x02 || psm == 0x0A || psm == 0x32 || psm == 0x3A) return {64, 64};
    if (psm == 0x13) return {128, 64};
    if (psm == 0x14) return {128, 128};
    return {64, 32};
}

// Calls f(page) for every page of the rectangle (x0, y0)-(x1, y1) inclusive of a buffer at `block` (64-word blocks) that is
// `bw` x 64 pixels wide. A buffer starting inside a page can spill into the next page: then that page is included too.
template <class F>
void for_pages(std::uint32_t block, std::uint32_t bw, std::uint32_t psm, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1, F &&f) {
    const PageSize ps = page_size(psm);
    const std::uint32_t per_row = std::max<std::uint32_t>(1, std::max<std::uint32_t>(bw, 1) * 64 / ps.w);
    const std::uint32_t first = block / 32, spill = block % 32 ? 1u : 0u;
    for (std::uint32_t py = y0 / ps.h; py <= y1 / ps.h; ++py)
        for (std::uint32_t px = x0 / ps.w; px <= x1 / ps.w + spill; ++px) f(first + py * per_row + px);
}

// how many pages a texture of 2^tw x 2^th texels at `block` with buffer width `bw` touches (for logging and checks)
inline std::uint32_t texture_rect_pages_count(std::uint32_t block, std::uint32_t bw, std::uint32_t psm, std::uint32_t tw, std::uint32_t th) {
    std::uint32_t n = 0;
    for_pages(block, bw, psm, 0, 0, (1u << tw) - 1, (1u << th) - 1, [&](std::uint32_t) { ++n; });
    return n;
}

}  // namespace ps2::gs::hwr
