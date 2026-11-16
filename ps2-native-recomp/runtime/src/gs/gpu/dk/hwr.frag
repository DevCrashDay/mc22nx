#version 460
// GPU triangle renderer: the GS pixel pipeline as far as the GPU does not do it in fixed function
// (docs/superpowers/plans/2026-11-15-switch-hw-renderer-stage2-3.md). GS values in 0..255 (colour 128 = 1.0 as factor).
// Output 0 = colour and source alpha; output 1 alpha = blend factor (dual-source blending, set by the backend).
// One exit, no `return` in main (Mesa compiles it to PRET, which Ryujinx cannot run); `discard` is fine.
layout(location = 0) in vec4 v_col;
layout(location = 1) in vec3 v_stq;
layout(location = 2) in float v_fog;

// state[0] = (shader bits, texture w | h << 16, AREF | FIX << 8, FOGCOL); state[1].x = 1/Zmax (vertex shader);
// state[0].x bits: 0 textured, 1 FST, 2 bilinear, 3 clamp, 4-5 TFX, 6 TCC, 7 fog, 8 alpha test, 9-11 ATST, 12-13 AFAIL,
// 14 factor = FIX, 15 no blending (factor 1), 16 debug red, 17 debug q, 18 texture is a render target (flip v), 19 output the factor as colour
layout(std140, binding = 0) uniform Draw {
    uvec4 state[4];
    vec4 size;
} u;

layout(binding = 0) uniform sampler2D tex;

layout(location = 0, index = 0) out vec4 frag;
layout(location = 0, index = 1) out vec4 frag_factor;

void main() {
    const uint s = u.state[0].x;
    const vec4 v = v_col * 255.0;
    vec4 c = v;
    if ((s & 1u) != 0u) {
        // state[0].y = the GS texture size; the image can be larger (a render target read as a texture)
        const vec2 tdim = vec2(float(u.state[0].y & 0xFFFFu), float(u.state[0].y >> 16));
        const vec2 isz = vec2(textureSize(tex, 0));
        vec2 uv = (s & 2u) != 0u ? v_stq.xy / isz : (v_stq.xy / v_stq.z) * tdim / isz;
        if ((s & 262144u) != 0u) uv.y = 1.0 - uv.y;  // a render target: its rows are stored bottom first
        const vec4 t = texture(tex, uv) * 255.0;
        const uint tfx = (s >> 4) & 3u;
        const bool tcc = (s & 64u) != 0u;
        if (tfx == 0u) {
            c = vec4(t.rgb * v.rgb / 128.0, tcc ? t.a * v.a / 128.0 : v.a);
        } else if (tfx == 1u) {
            c = vec4(t.rgb, tcc ? t.a : v.a);
        } else if (tfx == 2u) {
            c = vec4(t.rgb * v.rgb / 128.0 + v.a, tcc ? t.a + v.a : v.a);
        } else {
            c = vec4(t.rgb * v.rgb / 128.0 + v.a, tcc ? t.a : v.a);
        }
        c = min(c, vec4(255.0));
    }
    if ((s & 128u) != 0u) {
        const uint fc = u.state[0].w;
        const vec3 fog = vec3(float(fc & 0xFFu), float((fc >> 8) & 0xFFu), float((fc >> 16) & 0xFFu));
        c.rgb = mix(fog, c.rgb, v_fog);
    }
    float factor = (s & 32768u) != 0u ? 1.0 : ((s & 16384u) != 0u ? float((u.state[0].z >> 8) & 0xFFu) : c.a) / 128.0;
    factor = clamp(factor, 0.0, 1.0);
    float alpha_write = 1.0;
    if ((s & 256u) != 0u) {
        const float a = floor(c.a + 0.5), ref = float(u.state[0].z & 0xFFu);
        const uint atst = (s >> 9) & 7u;
        const bool pass = atst == 1u || (atst == 2u && a < ref) || (atst == 3u && a <= ref) || (atst == 4u && a == ref) ||
                          (atst == 5u && a >= ref) || (atst == 6u && a > ref) || (atst == 7u && a != ref);
        const uint afail = (s >> 12) & 3u;
        if (!pass && afail == 0u) discard;      // KEEP: nothing written
        if (!pass && afail == 2u) factor = 0.0;  // ZB_ONLY: depth only, the colour stays (factor 0)
        if (!pass && afail == 2u && u.state[1].y == 5u) {  // blend row 5 (Cd * f + Cs): colour 0, factor 1 keeps Cd
            c = vec4(0.0, 0.0, 0.0, c.a);
            factor = 1.0;
        }
        // FB_ONLY / RGB_ONLY must not write Z: with blending on (the transparent texels of foliage etc.) leave the pixel out
        if (!pass && (afail == 1u || afail == 3u) && (s & 32768u) == 0u) discard;
        if (!pass && (afail == 2u || afail == 3u)) alpha_write = 0.0;  // ZB_ONLY, RGB_ONLY: the frame's alpha stays
    }
    if ((s & 65536u) != 0u) c = vec4(255.0, 0.0, 0.0, 255.0);           // debug: constant red
    if ((s & 131072u) != 0u) c = vec4(v_stq.z * 255.0, 0.0, 0.0, 255.0);  // debug: q as red
    // The GS blends RGB only; the frame's alpha receives the source alpha. Output 1: rgb = the RGB blend factor (Src1Color),
    // a = 1 to write the source alpha / 0 to keep the frame's (alpha blends Src1Alpha, InvSrc1Alpha)
    frag = (s & 524288u) != 0u ? vec4(vec3(factor), c.a / 255.0) : c / 255.0;  // bit 19: the factor as colour (Cd * f + Cd)
    frag_factor = vec4(vec3(factor), alpha_write);
}
