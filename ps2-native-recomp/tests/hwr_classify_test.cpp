// PC test of runtime/src/gs/gpu/hwr/classify.hpp. Build and run (from the toolkit root):
//   clang++ -std=c++20 -I runtime/src -I runtime/include tests/hwr_classify_test.cpp -o hwr_classify && ./hwr_classify
#undef NDEBUG
#include "gs/gpu/hwr/classify.hpp"

#include <cassert>
#include <cstdio>

using namespace ps2::gs::hwr;

namespace {

ps2::gs::State base() {
    ps2::gs::State s{};
    s.prmodecont = 1;  // attributes from PRIM
    return s;
}

}  // namespace

int main() {
    {  // plain_textured_triangle
        ps2::gs::State s = base();
        s.prim = 3 | (1u << 4);
        const ShapeClass c = classify(s);
        assert(c.prim == kTri && (c.features & kTextured) && c.painter == 0);
    }
    {  // prmode_selects_attributes
        ps2::gs::State s = base();
        s.prmodecont = 0;
        s.prim = 3;
        s.prmode = 1u << 4;
        assert(classify(s).features & kTextured);
    }
    {  // context_two
        ps2::gs::State s = base();
        s.prim = 3 | (1u << 9);
        s.ctx[0].frame = 1;
        s.ctx[1].frame = 5;
        assert(classify(s).fbp == 5);
    }
    {  // line_goes_to_painter
        ps2::gs::State s = base();
        s.prim = 1;
        assert(classify(s).painter & kPainterPointLine);
    }
    {  // dest_alpha_blend
        ps2::gs::State s = base();
        s.prim = 3 | (1u << 6);
        s.ctx[0].alpha = 1u << 4;
        s.colclamp = 1;
        assert(classify(s).painter & kPainterDestAlphaBlend);
    }
    {  // partial_mask
        ps2::gs::State s = base();
        s.prim = 3;
        s.ctx[0].frame = 0x00F00000ull << 32;  // not whole bytes: painter
        assert(classify(s).painter & kPainterPartialMask);
        s.ctx[0].frame = 0x00FF0000ull << 32;  // whole bytes (blue kept): a colour write mask on the GPU
        assert(!(classify(s).painter & kPainterPartialMask));
        s.ctx[0].frame = 0;
        assert(!(classify(s).painter & kPainterPartialMask));
        s.ctx[0].frame = 0xFFFFFFFFull << 32;
        assert(!(classify(s).painter & kPainterPartialMask));
    }
    {  // region_wrap_and_stq
        ps2::gs::State s = base();
        s.prim = 3 | (1u << 4);  // textured, FST clear
        s.ctx[0].clamp = 2;      // WMS = REGION_CLAMP
        const std::uint32_t f = classify(s).features;
        assert((f & kRegionWrap) && (f & kStq));
    }
    {  // texture_from_target
        ps2::gs::State s = base();
        s.prim = 3 | (1u << 4);
        s.ctx[0].tex0 = 160;
        assert(texture_from_target(classify(s), 5));
        s.ctx[0].tex0 = 161;
        assert(!texture_from_target(classify(s), 5));
    }
    std::puts("hwr_classify_test: ok");
    return 0;
}
