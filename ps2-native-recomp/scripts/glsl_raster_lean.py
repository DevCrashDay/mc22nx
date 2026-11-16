#!/usr/bin/env python3
"""Raster shaders with fewer registers (deko3d build; uam's compiled raster used all 255 → one 256-thread group per SM):
1. the primitive's Setup (~50 words, the same for every thread of a tile) kept once per work group in shared memory
   instead of in every thread's registers. Every thread of a tile walks the same primitive list, so the setup changes at
   the same iteration for all of them (a uniform branch, safe for barrier());
2. sample_ rewritten with one texel read site and one format conversion for nearest (1 tap) and bilinear (4 taps), in a
   loop the compiler cannot unroll (each inlined read + conversion held its own registers); same reads, same blend4_;
3. at SCALE 1 rt_copy is always false: its branch (sample_scaled, more inlined reads) is dropped.

Register counts: HANDOFF.md. usage: glsl_raster_lean.py <in.comp> <out.comp>
"""
import sys


def convert(src):
    def rep(a, b):
        nonlocal src
        if src.count(a) != 1:
            sys.exit(f'glsl_raster_lean.py: expected once: {a!r} ({src.count(a)} times)')
        src = src.replace(a, b)

    # 8x8 tiles (gpu.cpp g_tile = 8 must match): 23 % fewer thread visits in the race (PS2_RASTER_STATS); on the Switch
    # (handheld, GPU 921 MHz) race 87-92 -> 75-78 ms/frame, picture clean. Known: Ryujinx draws red lines/dots in the race's
    # sky with 8x8 groups plus the shared setup below (8x8 alone is exact there; PC GL is exact) — an emulator issue so far.
    TILE8 = True
    if TILE8:
        rep('layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;', 'layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;')
        rep('const uint TILE = 16u;', 'const uint TILE = 8u;')
    rep('void main_body_() {', 'shared Setup sh_setup;  // glsl_raster_lean.py\n\nvoid main_body_() {')
    rep('''                Setup _e119 = load_setup((_e77 + (si * SETUP_WORDS)));
                s = _e119;''', '''                barrier();  // every thread is done with the previous setup
                if (gl_LocalInvocationIndex == 0u) {
                    sh_setup = load_setup((_e77 + (si * SETUP_WORDS)));
                }''')
    rep('''                    s.mip_at = (_e85 + (si * MIP_WORDS));
                    uint _e134 = s.mip_at;
                    uint _e136 = imageLoad(_group_0_binding_3_cs, int(_e134)).x;
                    s.mip = _e136;
                }''', '''                    if (gl_LocalInvocationIndex == 0u) {
                        sh_setup.mip_at = (_e85 + (si * MIP_WORDS));
                        sh_setup.mip = imageLoad(_group_0_binding_3_cs, int(sh_setup.mip_at)).x;
                    }
                }
                barrier();  // the new setup is in shared memory''')
    rep('''            Setup _e178 = s;
            shade_at(p_5, _e178,''', '''            shade_at(p_5, sh_setup,''')

    if 'const uint SCALE = 1u;' not in src:
        sys.exit('glsl_raster_lean.py: written for SCALE 1 (rt_copy is never set there)')
    start = src.index('uint sample_(Setup s_15, float uin_1, float vin_1, uint k_screen) {\n')
    end = src.index('\n}\n', start) + len('\n}\n')
    src = src[:start] + SAMPLE + src[end:]
    return src


# sample_ (gs.wgsl `sample`) with one texel read and one format conversion for every filter: nearest is 1 tap, bilinear 4,
# in a loop the compiler cannot unroll. Per tap the same coordinates, sample index, read (decoded copy / 32-bit fast path /
# general path) and conversion as the original; bilinear blends with the same blend4_. rt_copy (sample_scaled) is never set
# at SCALE 1.
SAMPLE = '''uint sample_(Setup s_15, float uin_1, float vin_1, uint k_screen) {
    // glsl_raster_lean.py: one read site, one conversion (each inlined copy held its own registers)
    bool drawn = drawn_format(s_15);
    bool bil = has(s_15, F_BILINEAR);
    float u_4 = bil ? (uin_1 - 0.5) : uin_1;
    float v_8 = bil ? (vin_1 - 0.5) : vin_1;
    int x0_1 = int(floor(u_4));
    int y0_1 = int(floor(v_8));
    uint xa = texel_x(s_15, x0_1);
    uint ya = texel_y(s_15, y0_1);
    uint xb = xa;
    uint yb = ya;
    uint kxa = 0u;
    uint kxb = 0u;
    uint kya = 0u;
    uint kyb = 0u;
    if (bil) {
        xb = texel_x(s_15, (x0_1 + 1));
        yb = texel_y(s_15, (y0_1 + 1));
        kxa = drawn ? sub_of(uin_1, x0_1) : (k_screen % SCALE);
        kxb = drawn ? sub_of(uin_1, (x0_1 + 1)) : (k_screen % SCALE);
        kya = drawn ? (sub_of(vin_1, y0_1) * SCALE) : (k_screen - (k_screen % SCALE));
        kyb = drawn ? (sub_of(vin_1, (y0_1 + 1)) * SCALE) : (k_screen - (k_screen % SCALE));
    } else {
        kya = drawn ? ((sub_of(vin_1, y0_1) * SCALE) + sub_of(uin_1, x0_1)) : k_screen;
    }
    bool fast32 = bil && s_15.dec == 4294967295u && s_15.tpsm <= 1u;
    uint c0_t = 0u;
    uint c1_t = 0u;
    uint c2_t = 0u;
    uint c3_t = 0u;
    uint taps_t = (bil ? 4u : 1u) >> (s_15.tw >> 31u);  // tw < 16: not known to the compiler, the loop stays a loop
    for (uint t = 0u; t < taps_t; t++) {
        uint xs = ((t & 1u) != 0u) ? xb : xa;
        uint ys = ((t & 2u) != 0u) ? yb : ya;
        uint kk = (((t & 2u) != 0u) ? kyb : kya) + (((t & 1u) != 0u) ? kxb : kxa);
        uint c = 0u;
        if (s_15.dec != 4294967295u) {
            c = imageLoad(_group_0_binding_7_cs, int(((s_15.dec + (ys << s_15.tw)) + xs))).x;
        } else {
            uint raw = 0u;
            if (fast32) {
                uint page = (((xs >> 6u) * 8192u) + s_15.tf.base) + (((ys >> 5u) * s_15.tf.per_row) * 8192u);
                uint word = (((page + ((spread_x(xs) | spread_y(ys)) << 2u)) & 4194303u) >> 2u);
                if (word >= snapshot_lo && word < snapshot_hi) {
                    raw = imageLoad(_group_0_binding_5_cs, int(((((word - snapshot_lo) * SCALE) * SCALE) + kk))).x;
                } else {
                    raw = imageLoad(_group_0_binding_0_cs, int((((word * SCALE) * SCALE) + kk))).x;
                }
                raw = raw & s_15.tf.mask;
            } else {
                raw = load_texel(addr(s_15.tf, xs, ys), kk);
            }
            c = convert(s_15.tpsm, s_15.flags, s_15.ta0_, s_15.ta1_, s_15.clut, s_15.cpsm, s_15.csa, raw);
        }
        if (t == 0u) {
            c0_t = c;
        } else if (t == 1u) {
            c1_t = c;
        } else if (t == 2u) {
            c2_t = c;
        } else {
            c3_t = c;
        }
    }
    if (!bil) {
        return c0_t;
    }
    return blend4_(c0_t, c1_t, c2_t, c3_t, (u_4 - float(x0_1)), (v_8 - float(y0_1)));
}
'''


# --texfetch: the GPU renderer's texture fetch (backend_deko3d.cpp / gpu.cpp hwr_fetch_texture): the same texel reads and
# format conversions as the painter (fetch_at), one texel per thread, written as RGBA8 words to binding 7.
# Parameters: prm(0) setup words in binding 3, prm(1) width, prm(2) height, prm(3) first output word.
# One exit, no early return (PRET; see dk/hwr.frag).
TEXFETCH_MAIN = '''void main_body_() {
    uint at = prm(0u);
    uint w = prm(1u);
    uint h = prm(2u);
    uint out_at = prm(3u);
    uint x = gl_GlobalInvocationID.x;
    uint y = gl_GlobalInvocationID.y;
    snapshot_lo = 0u;
    snapshot_hi = 0u;
    if (x < w && y < h) {
        Setup s = load_setup(at);
        uint c = fetch_at(s, x, y, 0u);
        imageStore(_group_0_binding_7_cs, int(out_at + y * w + x), uvec4(c, 0u, 0u, 0u));
    }
}
'''


def texfetch(src):
    start = src.index('void main_body_() {\n')
    end = src.index('void main() { main_body_(); }')
    return src[:start] + TEXFETCH_MAIN + '\n\n' + src[end:]


if __name__ == '__main__':
    with open(sys.argv[1], encoding='utf-8') as f:
        out = convert(f.read())
    if '--texfetch' in sys.argv[3:]:
        out = texfetch(out)
    with open(sys.argv[2], 'w', encoding='utf-8', newline='\n') as f:
        f.write(out)
