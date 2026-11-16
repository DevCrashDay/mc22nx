// PC test of runtime/src/gs/gpu/hwr/pages.hpp. Build and run (from the toolkit root):
//   clang++ -std=c++20 -I runtime/src -I runtime/include tests/hwr_pages_test.cpp -o hwr_pages && ./hwr_pages
#undef NDEBUG
#include "gs/gpu/hwr/pages.hpp"

#include <cassert>
#include <cstdio>
#include <set>

using namespace ps2::gs::hwr;

namespace {

std::set<std::uint32_t> pages(std::uint32_t block, std::uint32_t bw, std::uint32_t psm, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1) {
    std::set<std::uint32_t> out;
    for_pages(block, bw, psm, x0, y0, x1, y1, [&](std::uint32_t p) { out.insert(p); });
    return out;
}

}  // namespace

int main() {
    // 256x256 T8 texture at TBW 8 (512 pixels wide: 4 pages of 128 per row), 64-row pages: rows 0..3, columns 0..1
    assert((pages(0, 8, 0x13, 0, 0, 255, 255) == std::set<std::uint32_t>{0, 1, 4, 5, 8, 9, 12, 13}));
    // 32-bit frame, 640 wide (FBW 10), a 16x16 upload at (0, 256): page row 8 -> page 80 from base page 2
    assert((pages(64, 10, 0x00, 0, 256, 15, 271) == std::set<std::uint32_t>{2 + 80}));
    // 16-bit pages are 64x64
    assert((pages(0, 2, 0x02, 0, 0, 127, 127) == std::set<std::uint32_t>{0, 1, 2, 3}));
    // 4-bit pages are 128x128
    assert((pages(0, 4, 0x14, 0, 0, 255, 127) == std::set<std::uint32_t>{0, 1}));
    // a base inside a page also marks the next page (the rectangle can spill into it)
    assert((pages(33, 1, 0x00, 0, 0, 63, 31) == std::set<std::uint32_t>{1, 2}));
    // texture rectangle from TEX0: 64x32 CT32 at TBW 1 -> one page
    assert((texture_rect_pages_count(0, 1, 0x00, 6, 5) == 1));
    std::puts("hwr_pages_test: ok");
    return 0;
}
