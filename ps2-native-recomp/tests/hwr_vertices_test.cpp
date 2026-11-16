// PC test of runtime/src/gs/gpu/hwr/vertices.hpp. Build and run (from the toolkit root):
//   clang++ -std=c++20 -I runtime/src -I runtime/include tests/hwr_vertices_test.cpp -o hwr_vertices && ./hwr_vertices
#undef NDEBUG
#include "gs/gpu/hwr/vertices.hpp"

#include <cassert>
#include <cstdio>

using namespace ps2::gs::hwr;
using ps2::gs::gpu::be::HwrVertex;

namespace {

ps2::gs::State base(std::uint64_t prim) {
    ps2::gs::State s{};
    s.prmodecont = 1;
    s.prim = prim;
    s.ctx[0].xyoffset = (2048ull * 16) | ((2048ull * 16) << 32);
    return s;
}

ps2::gs::Vertex at(int x, int y) {
    ps2::gs::Vertex v{};
    v.x = static_cast<std::uint32_t>((2048 + x) * 16);
    v.y = static_cast<std::uint32_t>((2048 + y) * 16);
    v.q = 1.0f;
    return v;
}

}  // namespace

int main() {
    HwrVertex out[6];
    {  // tri_positions
        const ps2::gs::State s = base(3);
        const ps2::gs::Vertex v[3] = {at(10, 0), at(20, 0), at(10, 5)};
        assert(convert_shape(s, classify(s), v, 3, out) == 3);
        assert(out[0].x == 10.0f && out[0].y == 0.0f && out[2].y == 5.0f && out[0].w == 0.0f);
    }
    {  // sprite_two_triangles: the second vertex's colour everywhere, both corners covered
        const ps2::gs::State s = base(6);
        ps2::gs::Vertex v[2] = {at(0, 0), at(16, 8)};
        v[0].r = 1, v[1].r = 200, v[1].a = 128;
        assert(convert_shape(s, classify(s), v, 2, out) == 6);
        bool tl = false, br = false;
        for (const HwrVertex &o : out) {
            assert(o.rgba == (200u | (128u << 24)));
            tl |= o.x == 0.0f && o.y == 0.0f;
            br |= o.x == 16.0f && o.y == 8.0f;
        }
        assert(tl && br);
    }
    {  // fst_uv: texel coordinates (u/16), q = 1
        const ps2::gs::State s = base(3 | (1u << 4) | (1u << 8));
        ps2::gs::Vertex v[3] = {at(0, 0), at(1, 0), at(0, 1)};
        v[0].u = 32, v[0].v = 48;
        assert(convert_shape(s, classify(s), v, 3, out) == 3);
        assert(out[0].s == 2.0f && out[0].t == 3.0f && out[0].q == 1.0f);
    }
    {  // stq_passthrough
        const ps2::gs::State s = base(3 | (1u << 4));
        ps2::gs::Vertex v[3] = {at(0, 0), at(1, 0), at(0, 1)};
        v[0].s = 0.5f, v[0].t = 0.25f, v[0].q = 2.0f;
        assert(convert_shape(s, classify(s), v, 3, out) == 3);
        assert(out[0].s == 0.5f && out[0].t == 0.25f && out[0].q == 2.0f);
    }
    {  // flat_shading: IIP clear -> every vertex takes the last vertex's colour
        const ps2::gs::State s = base(3);
        ps2::gs::Vertex v[3] = {at(0, 0), at(1, 0), at(0, 1)};
        v[0].g = 5, v[2].g = 77;
        convert_shape(s, classify(s), v, 3, out);
        assert(((out[0].rgba >> 8) & 0xFF) == 77);
    }
    {  // gouraud: IIP set keeps each vertex's colour
        const ps2::gs::State s = base(3 | (1u << 3));
        ps2::gs::Vertex v[3] = {at(0, 0), at(1, 0), at(0, 1)};
        v[0].g = 5, v[2].g = 77;
        convert_shape(s, classify(s), v, 3, out);
        assert(((out[0].rgba >> 8) & 0xFF) == 5);
    }
    {  // fog_in_w: the vertex fog value (0..255) as 0..1 in w
        const ps2::gs::State s = base(3);
        ps2::gs::Vertex v[3] = {at(0, 0), at(1, 0), at(0, 1)};
        v[0].f = 255, v[1].f = 0;
        convert_shape(s, classify(s), v, 3, out);
        assert(out[0].w == 1.0f && out[1].w == 0.0f);
    }
    {  // lines and points are not converted (painter)
        const ps2::gs::State s = base(1);
        const ps2::gs::Vertex v[2] = {at(0, 0), at(5, 5)};
        assert(convert_shape(s, classify(s), v, 2, out) == 0);
    }
    std::puts("hwr_vertices_test: ok");
    return 0;
}
