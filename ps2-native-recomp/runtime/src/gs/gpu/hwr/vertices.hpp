#pragma once
// GS shapes as GPU triangles for the GPU renderer (backend.hpp HwrVertex). Pure; tests/hwr_vertices_test.cpp checks it.
// gs.cpp hands over one shape per call: 3 vertices for triangles, strips and fans, 2 corners for sprites.

#include "../backend.hpp"
#include "classify.hpp"

#include <cstdint>

namespace ps2::gs::hwr {

inline std::uint32_t pack_rgba(const ps2::gs::Vertex &v) {
    return static_cast<std::uint32_t>(v.r) | (static_cast<std::uint32_t>(v.g) << 8) | (static_cast<std::uint32_t>(v.b) << 16) |
           (static_cast<std::uint32_t>(v.a) << 24);
}

// Writes a triangle list (3 or 6 vertices) to `out` and returns how many; 0 for points and lines (painter).
// Positions in target pixels (GS 12.4 minus XYOFFSET), z raw (the vertex shader scales it), w = fog 0..1, colour RGBA8 (flat shading: the last
// vertex's colour, as the GS does), texture coordinates in texels with FST (u, v / 16, q = 1), else s, t, q as given.
inline std::uint32_t convert_shape(const ps2::gs::State &s, const ShapeClass &c, const ps2::gs::Vertex *v, int count,
                                   ps2::gs::gpu::be::HwrVertex *out) {
    using ps2::gs::gpu::be::HwrVertex;
    const std::uint64_t attr = (s.prmodecont & 1) ? s.prim : s.prmode;
    const ps2::gs::Context &x = s.ctx[(attr >> 9) & 1];
    const float ofx = static_cast<float>(x.xyoffset & 0xFFFF) / 16.0f, ofy = static_cast<float>((x.xyoffset >> 32) & 0xFFFF) / 16.0f;
    const bool fst = (attr >> 8) & 1, gouraud = (attr >> 3) & 1;
    const auto make = [&](const ps2::gs::Vertex &p, const ps2::gs::Vertex &colour, const ps2::gs::Vertex &tex) {
        HwrVertex o{};
        o.x = static_cast<float>(p.x) / 16.0f - ofx;
        o.y = static_cast<float>(p.y) / 16.0f - ofy;
        o.z = static_cast<float>(p.z);
        o.w = static_cast<float>(p.f) / 255.0f;  // fog (dk/hwr.vert passes it on; the GS mixes FOGCOL by 1 - f/255)
        o.rgba = pack_rgba(colour);
        if (fst) o.s = static_cast<float>(tex.u) / 16.0f, o.t = static_cast<float>(tex.v) / 16.0f, o.q = 1.0f;
        else o.s = tex.s, o.t = tex.t, o.q = tex.q;
        return o;
    };
    if (c.prim == kSprite && count >= 2) {
        // corners a (top left) and b (bottom right); colour, z and q of the second vertex, as the GS draws sprites
        ps2::gs::Vertex tl = v[1], tr = v[1], br = v[1], bl = v[1];
        tl.x = v[0].x, tl.y = v[0].y, tl.u = v[0].u, tl.v = v[0].v, tl.s = v[0].s, tl.t = v[0].t;
        tr.y = v[0].y, tr.v = v[0].v, tr.t = v[0].t;
        bl.x = v[0].x, bl.u = v[0].u, bl.s = v[0].s;
        out[0] = make(tl, v[1], tl), out[1] = make(tr, v[1], tr), out[2] = make(br, v[1], br);
        out[3] = out[0], out[4] = out[2], out[5] = make(bl, v[1], bl);
        return 6;
    }
    if (c.prim >= kTri && c.prim <= kTriFan && count >= 3) {
        for (int i = 0; i < 3; ++i) out[i] = make(v[i], gouraud ? v[i] : v[2], v[i]);
        return 3;
    }
    return 0;
}

}  // namespace ps2::gs::hwr
