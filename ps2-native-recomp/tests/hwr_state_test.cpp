// PC test of runtime/src/gs/gpu/hwr/state.hpp. Build and run (from the toolkit root):
//   clang++ -std=c++20 -I runtime/src -I runtime/include tests/hwr_state_test.cpp -o hwr_state && ./hwr_state
#undef NDEBUG
#include "gs/gpu/hwr/state.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace ps2::gs::hwr;

namespace {

ps2::gs::State shape(std::uint64_t prim_bits) {
    ps2::gs::State s{};
    s.prmodecont = 1;
    s.prim = 3 | prim_bits;
    s.colclamp = 1;
    return s;
}

std::uint64_t alpha(unsigned a, unsigned b, unsigned c, unsigned d, unsigned fix = 0) {
    return a | (b << 2) | (c << 4) | (d << 6) | (static_cast<std::uint64_t>(fix) << 32);
}

GpuState map(const ps2::gs::State &s) { return map_state(s, classify(s)); }

}  // namespace

int main() {
    const std::uint64_t abe = 1u << 6;
    {  // blend rows
        ps2::gs::State s = shape(abe);
        s.ctx[0].alpha = alpha(0, 1, 0, 1);
        assert(map(s).blend == kBlendLerp && !(map(s).shader[0] & kShNoBlend));
        s.ctx[0].alpha = alpha(0, 2, 0, 1);
        assert(map(s).blend == kBlendAdd);
        s.ctx[0].alpha = alpha(2, 0, 0, 1);
        assert(map(s).blend == kBlendRevSub);
        s.ctx[0].alpha = alpha(0, 2, 0, 2);
        assert(map(s).blend == kBlendSourceOnly);
        s.ctx[0].alpha = alpha(1, 2, 0, 0);  // Cd * As + Cs (MC2's race: most blended shapes)
        assert(map(s).blend == kBlendDstScaleAdd);
        s.ctx[0].alpha = alpha(1, 0, 0, 2);  // (Cd - Cs) * As
        assert(map(s).blend == kBlendDiffScaled);
        s.ctx[0].alpha = alpha(1, 2, 0, 1);  // Cd * As + Cd: the shader outputs the factor, blended DstColor + One
        assert(map(s).blend == kBlendDstBrighten && (map(s).shader[0] & kShOutFactor));
        s.ctx[0].alpha = alpha(1, 0, 0, 0);
        assert(map(s).blend == kBlendUnmapped);
        s.ctx[0].alpha = alpha(0, 1, 2, 1, 0x80);  // C = FIX
        const GpuState g = map(s);
        assert((g.shader[0] & kShFixFactor) && ((g.shader[2] >> 8) & 0xFF) == 0x80);
    }
    {  // abe_off: lerp with factor 1
        ps2::gs::State s = shape(0);
        const GpuState g = map(s);
        assert(g.blend == kBlendLerp && (g.shader[0] & kShNoBlend));
    }
    {  // depth tests
        ps2::gs::State s = shape(0);
        s.ctx[0].test = 1u << 16;  // ZTE, ZTST 0 = NEVER
        assert(map(s).ztest == kZNever && map(s).zwrite);
        s.ctx[0].test = (1u << 16) | (1u << 17);
        assert(map(s).ztest == kZAlways);
        s.ctx[0].test = (1u << 16) | (2u << 17);
        assert(map(s).ztest == kZGequal);
        s.ctx[0].test = (1u << 16) | (3u << 17);
        assert(map(s).ztest == kZGreater);
        s.ctx[0].zbuf = 1ull << 32;  // ZMSK
        assert(!map(s).zwrite);
        s.ctx[0].test = 0;
        assert(map(s).ztest == kZOff && !map(s).zwrite);
    }
    {  // alpha test fields and AREF
        ps2::gs::State s = shape(0);
        s.ctx[0].test = 1u | (4u << 1) | (0x40u << 4) | (2u << 12);  // ATE, ATST = GEQUAL, AREF 0x40, AFAIL = ZB_ONLY
        const GpuState g = map(s);
        assert((g.shader[0] & kShAlphaTest) && ((g.shader[0] >> 9) & 7) == 4 && ((g.shader[0] >> 12) & 3) == 2 && (g.shader[2] & 0xFF) == 0x40);
    }
    {  // colour write mask from whole-byte FBMSK: bit set = channel written (R 1, G 2, B 4, A 8)
        ps2::gs::State s = shape(0);
        assert(map(s).write_mask == 0xF);
        s.ctx[0].frame = 0xFF000000ull << 32;  // alpha kept
        assert(map(s).write_mask == 0x7);
        s.ctx[0].frame = 0x0000FF00ull << 32;  // green kept
        assert(map(s).write_mask == 0xD);
    }
    {  // z scale by format: Z24 -> 1 / 2^24
        ps2::gs::State s = shape(0);
        s.ctx[0].zbuf = 1ull << 24;  // ZPSM 1 = Z24
        float f;
        const GpuState g = map(s);
        std::memcpy(&f, &g.shader[4], 4);
        assert(f == 1.0f / 16777216.0f);
    }
    {  // texture function and fog
        ps2::gs::State s = shape((1u << 4) | (1u << 5));  // TME, FGE
        s.ctx[0].tex0 = (1ull << 34) | (1ull << 35) | (6ull << 26) | (5ull << 30);  // TCC, TFX = DECAL, 64 x 32
        s.fogcol = 0x00112233;
        const GpuState g = map(s);
        assert((g.shader[0] & kShTextured) && (g.shader[0] & kShTcc) && ((g.shader[0] >> 4) & 3) == 1 && (g.shader[0] & kShFog));
        assert(g.shader[1] == (64u | (32u << 16)) && g.shader[3] == 0x00112233u);
    }
    std::puts("hwr_state_test: ok");
    return 0;
}
