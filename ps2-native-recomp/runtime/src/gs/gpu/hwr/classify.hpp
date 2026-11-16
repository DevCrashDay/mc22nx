#pragma once
// The GPU renderer's view of one GS shape (docs/superpowers/specs/2026-11-15-switch-hw-renderer-design.md): which
// primitive, which features it uses, and why it would have to stay on the painter. Pure: reads the GS registers in
// ps2::gs::State (bit layout per the GS manual), allocates nothing; tests/hwr_classify_test.cpp checks it on the PC.

#include "../../gs.hpp"

#include <cstdint>

namespace ps2::gs::hwr {

enum Prim : std::uint8_t { kPoint, kLine, kLineStrip, kTri, kTriStrip, kTriFan, kSprite };

enum Painter : std::uint32_t {  // reasons a shape stays on the painter
    kPainterPointLine = 1,
    kPainterDestAlphaBlend = 2,  // ALPHA.C == 1: blend factor from the destination alpha
    kPainterDestAlphaTest = 4,   // TEST.DATE
    kPainterPartialMask = 8,     // FBMSK not whole bytes (whole-byte masks are a colour write mask on the GPU)
    kPainterFrameFormat = 16,    // frame PSM not CT32/CT24/CT16/CT16S
    kPainterWrapBlend = 32,      // blending with COLCLAMP off (colour wrap-around)
    kPainterFba = 64,            // FBA
};

enum Feature : std::uint32_t {
    kTextured = 1,
    kBlend = 2,
    kAlphaTest = 4,
    kDepthTest = 8,
    kDepthWrite = 16,
    kFog = 32,
    kMip = 64,          // TEX1.MXL > 0 and MMIN >= 2
    kBilinear = 128,    // TEX1.MMAG
    kStq = 256,         // perspective S/T/Q (FST clear)
    kRegionWrap = 512,  // CLAMP.WMS or WMT is REGION_CLAMP / REGION_REPEAT
    kPalette = 1024,    // texture PSM T8 / T4 / T8H / T4HL / T4HH
};

struct ShapeClass {
    Prim prim = kTri;
    std::uint32_t painter = 0;
    std::uint32_t features = 0;
    std::uint32_t fbp = 0, fbw = 0, fpsm = 0, zbp = 0, tbp = 0, tpsm = 0;
};

inline std::uint32_t bits(std::uint64_t v, unsigned lo, unsigned n) { return static_cast<std::uint32_t>((v >> lo) & ((1ull << n) - 1)); }

inline ShapeClass classify(const ps2::gs::State &s) {
    ShapeClass c;
    c.prim = static_cast<Prim>(bits(s.prim, 0, 3));
    // TME, FGE, ABE, FST (and CTXT) come from PRMODE when PRMODECONT is 0
    const std::uint64_t attr = (s.prmodecont & 1) ? s.prim : s.prmode;
    const bool tme = bits(attr, 4, 1), fge = bits(attr, 5, 1), abe = bits(attr, 6, 1), fst = bits(attr, 8, 1);
    const ps2::gs::Context &x = s.ctx[bits(attr, 9, 1)];

    c.fbp = bits(x.frame, 0, 9);
    c.fbw = bits(x.frame, 16, 6);
    c.fpsm = bits(x.frame, 24, 6);
    c.zbp = bits(x.zbuf, 0, 9);
    c.tbp = bits(x.tex0, 0, 14);
    c.tpsm = bits(x.tex0, 20, 6);

    if (c.prim <= kLineStrip) c.painter |= kPainterPointLine;
    if (abe && bits(x.alpha, 4, 2) == 1) c.painter |= kPainterDestAlphaBlend;
    if (bits(x.test, 14, 1)) c.painter |= kPainterDestAlphaTest;
    if (const std::uint32_t m = bits(x.frame, 32, 32); m != 0 && m != 0xFFFFFFFFu) {
        bool whole_bytes = true;
        for (int i = 0; i < 4; ++i) whole_bytes &= ((m >> (i * 8)) & 0xFF) == 0 || ((m >> (i * 8)) & 0xFF) == 0xFF;
        if (!whole_bytes) c.painter |= kPainterPartialMask;
    }
    if (c.fpsm != 0x00 && c.fpsm != 0x01 && c.fpsm != 0x02 && c.fpsm != 0x0A) c.painter |= kPainterFrameFormat;
    if (abe && (s.colclamp & 1) == 0) c.painter |= kPainterWrapBlend;
    if (x.fba & 1) c.painter |= kPainterFba;

    if (tme) {
        c.features |= kTextured;
        if (!fst) c.features |= kStq;
        if (bits(x.tex1, 2, 3) > 0 && bits(x.tex1, 6, 3) >= 2) c.features |= kMip;
        if (bits(x.tex1, 5, 1)) c.features |= kBilinear;
        if (bits(x.clamp, 0, 2) >= 2 || bits(x.clamp, 2, 2) >= 2) c.features |= kRegionWrap;
        if (c.tpsm == 0x13 || c.tpsm == 0x14 || c.tpsm == 0x1B || c.tpsm == 0x24 || c.tpsm == 0x2C) c.features |= kPalette;
    }
    if (abe) c.features |= kBlend;
    if (bits(x.test, 0, 1)) c.features |= kAlphaTest;
    if (bits(x.test, 16, 1)) {
        if (bits(x.test, 17, 2) != 1) c.features |= kDepthTest;  // ZTST 1 = ALWAYS
        if (!bits(x.zbuf, 32, 1)) c.features |= kDepthWrite;
    }
    if (fge) c.features |= kFog;
    return c;
}

// TBP0 counts 64-word blocks, FBP 2048-word pages: a texture at a frame buffer's address reads what was drawn there
inline bool texture_from_target(const ShapeClass &c, std::uint32_t target_fbp) {
    return (c.features & kTextured) && c.tbp == target_fbp * 32;
}

}  // namespace ps2::gs::hwr
