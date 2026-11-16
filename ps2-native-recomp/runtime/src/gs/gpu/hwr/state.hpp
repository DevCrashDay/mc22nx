#pragma once
// GS drawing state -> the GPU renderer's state (docs/superpowers/plans/2026-11-15-switch-hw-renderer-stage2-3.md, Task 1).
// Pure; tests/hwr_state_test.cpp checks it. The uber shader (dk/hwr.frag) reads `shader`; the backend sets blend and depth.

#include "classify.hpp"

#include <cstdint>
#include <cstring>

namespace ps2::gs::hwr {

enum ShaderBits : std::uint32_t {
    kShTextured = 1u << 0,
    kShFst = 1u << 1,
    kShBilinear = 1u << 2,
    kShClamp = 1u << 3,
    // bits 4-5: TFX (0 modulate, 1 decal, 2 highlight, 3 highlight2)
    kShTcc = 1u << 6,
    kShFog = 1u << 7,
    kShAlphaTest = 1u << 8,
    // bits 9-11: ATST, bits 12-13: AFAIL
    kShFixFactor = 1u << 14,  // blend factor C = FIX (else As)
    kShNoBlend = 1u << 15,    // ABE off: factor 1
    // bits 16, 17: debug colours (dk/hwr.frag)
    kShFlipV = 1u << 18,      // the texture is a render target (stored bottom row first): flip v
    kShOutFactor = 1u << 19,  // output 0 colour = the blend factor (kBlendDstBrighten)
};

// Dual-source blending: output 1 alpha is the factor C/128; every row writes Cv = (A - B) * C + D in fixed function.
enum Blend : std::uint8_t {
    kBlendLerp,        // (Cs - Cd) * C + Cd: Src1Alpha, InvSrc1Alpha, Add
    kBlendAdd,         // (Cs - 0) * C + Cd:  Src1Alpha, One, Add
    kBlendRevSub,      // (0 - Cs) * C + Cd:  Src1Alpha, One, ReverseSubtract
    kBlendSourceOnly,  // (Cs - 0) * C + 0:   Src1Alpha, Zero, Add
    kBlendUnmapped,    // anything else: drawn as lerp (counted)
    kBlendDstScaleAdd, // (Cd - 0) * C + Cs:  One, Src1Alpha, Add (MC2's race: most blended shapes)
    kBlendDiffScaled,  // (Cd - Cs) * C + 0:  Src1Alpha, Src1Alpha, ReverseSubtract (dst*f - src*f)
    kBlendDstBrighten, // (Cd - 0) * C + Cd:  shader outputs f; DstColor, One, Add
};

enum ZTest : std::uint8_t { kZOff, kZNever, kZAlways, kZGequal, kZGreater };

struct GpuState {
    std::uint32_t shader[8]{};
    std::uint8_t blend = kBlendLerp, ztest = kZOff;
    bool zwrite = false;
    std::uint8_t write_mask = 0xF;  // channels written: R 1, G 2, B 4, A 8 (FBMSK bytes that are 0)
    std::uint32_t zbp = 0, zpsm = 0;
};

inline GpuState map_state(const ps2::gs::State &s, const ShapeClass &c) {
    GpuState g;
    const std::uint64_t attr = (s.prmodecont & 1) ? s.prim : s.prmode;
    const ps2::gs::Context &x = s.ctx[(attr >> 9) & 1];
    std::uint32_t &sh = g.shader[0];
    if (c.features & kTextured) {
        sh |= kShTextured;
        if (bits(attr, 8, 1)) sh |= kShFst;
        if (c.features & kBilinear) sh |= kShBilinear;
        if (bits(x.clamp, 0, 2) == 1 || bits(x.clamp, 2, 2) == 1) sh |= kShClamp;
        sh |= bits(x.tex0, 35, 2) << 4;
        if (bits(x.tex0, 34, 1)) sh |= kShTcc;
        g.shader[1] = (1u << bits(x.tex0, 26, 4)) | ((1u << bits(x.tex0, 30, 4)) << 16);
    }
    if (c.features & kFog) sh |= kShFog;
    const std::uint32_t fbmsk = bits(x.frame, 32, 32);
    g.write_mask = 0;
    for (int i = 0; i < 4; ++i)
        if (((fbmsk >> (i * 8)) & 0xFF) == 0) g.write_mask |= 1u << i;
    if (bits(x.test, 0, 1)) sh |= kShAlphaTest | (bits(x.test, 1, 3) << 9) | (bits(x.test, 12, 2) << 12);
    g.shader[2] = bits(x.test, 4, 8) | (bits(x.alpha, 32, 8) << 8);
    g.shader[3] = static_cast<std::uint32_t>(s.fogcol & 0xFFFFFF);
    g.zpsm = bits(x.zbuf, 24, 4);
    g.zbp = bits(x.zbuf, 0, 9);
    const float zmax = g.zpsm == 0 ? 4294967296.0f : g.zpsm == 1 ? 16777216.0f : 65536.0f;
    const float zscale = 1.0f / zmax;
    std::memcpy(&g.shader[4], &zscale, 4);

    if (!(c.features & kBlend)) {
        g.blend = kBlendLerp;
        sh |= kShNoBlend;
    } else {
        const std::uint32_t a = bits(x.alpha, 0, 2), b = bits(x.alpha, 2, 2), cc = bits(x.alpha, 4, 2), d = bits(x.alpha, 6, 2);
        if (cc == 2) sh |= kShFixFactor;
        if (a == 0 && b == 1 && d == 1) g.blend = kBlendLerp;
        else if (a == 0 && b == 2 && d == 1) g.blend = kBlendAdd;
        else if (a == 2 && b == 0 && d == 1) g.blend = kBlendRevSub;
        else if (a == 0 && b == 2 && d == 2) g.blend = kBlendSourceOnly;
        else if (a == 1 && b == 2 && d == 0) g.blend = kBlendDstScaleAdd;
        else if (a == 1 && b == 0 && d == 2) g.blend = kBlendDiffScaled;
        else if (a == 1 && b == 2 && d == 1) g.blend = kBlendDstBrighten, sh |= kShOutFactor;
        else g.blend = kBlendUnmapped;
    }
    if (bits(x.test, 16, 1)) {
        const std::uint32_t t = bits(x.test, 17, 2);
        g.ztest = t == 0 ? kZNever : t == 1 ? kZAlways : t == 2 ? kZGequal : kZGreater;
        g.zwrite = !bits(x.zbuf, 32, 1);
    }
    return g;
}

}  // namespace ps2::gs::hwr
