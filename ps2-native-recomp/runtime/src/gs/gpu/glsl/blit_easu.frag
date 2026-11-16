#version 430 core
// glsl_compat.py: converted for nouveau and Ryujinx (see the script)
#extension GL_ARB_shader_storage_buffer_object : require
struct Blit {
    uint width;
    uint height;
    uint current;
    uint previous;
    uint blend;
    uint out_width;
    uint out_height;
    uint options;
};
struct VertexOut {
    vec4 position;
    vec2 uv;
};
const float SHARPNESS = 0.14;
const float EDGE_SLANT = 0.04;
const float EDGE_REACH = 6.0;

layout(r32ui, binding = 0) uniform readonly uimageBuffer _group_0_binding_0_fs;

layout(std140, binding = 1) uniform Blit_block_1Fragment { Blit _group_0_binding_1_fs; };

layout(location = 0) smooth in vec2 _vs2fs_location0;
layout(location = 0) out vec4 _fs2p_location0;

vec4 texel(uint base, int x, int y) {
    uint _e5 = _group_0_binding_1_fs.width;
    uint cx = uint(min(max(x, 0), (int(_e5) - 1)));
    uint _e14 = _group_0_binding_1_fs.height;
    uint cy = uint(min(max(y, 0), (int(_e14) - 1)));
    uint _e24 = _group_0_binding_1_fs.width;
    uint _e29 = imageLoad(_group_0_binding_0_fs, int(((base + (cy * _e24)) + cx))).x;
    return unpackUnorm4x8(_e29);
}

vec4 sample_(uint base_1, vec2 uv) {
    vec2 p = vec2(0.0);
    uint _e4 = _group_0_binding_1_fs.width;
    uint _e8 = _group_0_binding_1_fs.height;
    vec2 size = vec2(float(_e4), float(_e8));
    vec2 t = (uv * size);
    uint _e15 = _group_0_binding_1_fs.options;
    if (((_e15 & 6u) != 0u)) {
        p = (t - vec2(0.5));
    } else {
        uint _e25 = _group_0_binding_1_fs.out_width;
        uint _e29 = _group_0_binding_1_fs.out_height;
        vec2 scale = max(floor((vec2(float(_e25), float(_e29)) / size)), vec2(1.0));
        vec2 d = (fract(t) - vec2(0.5));
        vec2 region = (vec2(0.5) - (vec2(0.5) / scale));
        p = (floor(t) + ((d - clamp(d, -(region), region)) * scale));
    }
    vec2 _e53 = p;
    vec2 origin = floor(_e53);
    vec2 _e55 = p;
    vec2 f = (_e55 - origin);
    int x_2 = int(origin.x);
    int y_2 = int(origin.y);
    vec4 _e61 = texel(base_1, x_2, y_2);
    vec4 _e64 = texel(base_1, (x_2 + 1), y_2);
    vec4 top = mix(_e61, _e64, f.x);
    vec4 _e69 = texel(base_1, x_2, (y_2 + 1));
    vec4 _e74 = texel(base_1, (x_2 + 1), (y_2 + 1));
    vec4 bottom = mix(_e69, _e74, f.x);
    return mix(top, bottom, f.y);
}

vec3 scene(vec2 uv_1) {
    vec3 c = vec3(0.0);
    uint _e3 = _group_0_binding_1_fs.current;
    vec4 _e4 = sample_(_e3, uv_1);
    c = _e4.xyz;
    uint _e9 = _group_0_binding_1_fs.blend;
    if ((_e9 != 0u)) {
        vec3 _e12 = c;
        uint _e15 = _group_0_binding_1_fs.previous;
        vec4 _e16 = sample_(_e15, uv_1);
        c = ((_e12 + _e16.xyz) * 0.5);
    }
    vec3 _e21 = c;
    return _e21;
}

vec3 scene_texel(int x_1, int y_1) {
    vec3 c_1 = vec3(0.0);
    uint _e4 = _group_0_binding_1_fs.current;
    vec4 _e5 = texel(_e4, x_1, y_1);
    c_1 = _e5.xyz;
    uint _e10 = _group_0_binding_1_fs.blend;
    if ((_e10 != 0u)) {
        vec3 _e13 = c_1;
        uint _e16 = _group_0_binding_1_fs.previous;
        vec4 _e17 = texel(_e16, x_1, y_1);
        c_1 = ((_e13 + _e17.xyz) * 0.5);
    }
    vec3 _e22 = c_1;
    return _e22;
}

float easu_luma(vec3 c_2) {
    return ((c_2.z * 0.5) + ((c_2.x * 0.5) + c_2.y));
}

void easu_set(float w, float la, float lb, float lc, float ld, float le, inout vec2 dir_1, inout float len_1) {
    float lenx = 0.0;
    float leny = 0.0;
    float dc = (ld - lc);
    float cb = (lc - lb);
    lenx = max(abs(dc), abs(cb));
    float _e14 = lenx;
    lenx = (1.0 / max(_e14, 1e-8));
    float dirx = (ld - lb);
    float _e21 = lenx;
    lenx = clamp((abs(dirx) * _e21), 0.0, 1.0);
    float _e26 = lenx;
    float _e27 = lenx;
    lenx = (_e26 * _e27);
    float ec = (le - lc);
    float ca = (lc - la);
    leny = max(abs(ec), abs(ca));
    float _e35 = leny;
    leny = (1.0 / max(_e35, 1e-8));
    float diry = (le - la);
    float _e42 = leny;
    leny = clamp((abs(diry) * _e42), 0.0, 1.0);
    float _e47 = leny;
    float _e48 = leny;
    leny = (_e47 * _e48);
    vec2 _e50 = dir_1;
    dir_1 = (_e50 + (vec2(dirx, diry) * w));
    float _e54 = len_1;
    float _e55 = lenx;
    float _e56 = leny;
    len_1 = (_e54 + ((_e55 + _e56) * w));
    return;
}

void easu_tap(vec2 off, vec2 dir_2, vec2 len2_, float lob, float clp, vec3 c_3, inout vec4 acc_1) {
    vec2 v = vec2(0.0);
    float wb = 0.0;
    float wa = 0.0;
    v = vec2(((off.x * dir_2.x) + (off.y * dir_2.y)), ((off.x * -(dir_2.y)) + (off.y * dir_2.x)));
    vec2 _e24 = v;
    v = (_e24 * len2_);
    float _e27 = v.x;
    float _e29 = v.x;
    float _e32 = v.y;
    float _e34 = v.y;
    float d2_ = min(((_e27 * _e29) + (_e32 * _e34)), clp);
    wb = ((0.4 * d2_) - 1.0);
    wa = ((lob * d2_) - 1.0);
    float _e47 = wb;
    float _e48 = wb;
    wb = (_e47 * _e48);
    float _e50 = wa;
    float _e51 = wa;
    wa = (_e50 * _e51);
    float _e53 = wb;
    wb = ((1.5625 * _e53) - 0.5625);
    float _e58 = wb;
    float _e59 = wa;
    float w_1 = (_e58 * _e59);
    vec4 _e61 = acc_1;
    acc_1 = (_e61 + vec4((c_3 * w_1), w_1));
    return;
}

void main_body_() {
    VertexOut in_ = VertexOut(gl_FragCoord, _vs2fs_location0);
    bool local = false;
    vec2 pp = vec2(0.0);
    vec2 dir = vec2(0.0);
    float len = 0.0;
    float dirr = 0.0;
    vec4 acc = vec4(0.0);
    uint _e3 = _group_0_binding_1_fs.width;
    uint _e7 = _group_0_binding_1_fs.height;
    vec2 size_1 = vec2(float(_e3), float(_e7));
    uint _e12 = _group_0_binding_1_fs.out_width;
    uint _e16 = _group_0_binding_1_fs.out_height;
    vec2 out_size = vec2(float(_e12), float(_e16));
    if (!((out_size.x < size_1.x))) {
        local = (out_size.y < size_1.y);
    } else {
        local = true;
    }
    bool _e29 = local;
    if (_e29) {
        vec3 _e33 = scene((in_.position.xy / out_size));
        _fs2p_location0 = vec4(clamp(_e33, vec3(0.0), vec3(1.0)), 1.0);
        return;
    }
    pp = (((in_.position.xy * size_1) / out_size) - vec2(0.5));
    vec2 _e49 = pp;
    vec2 fp = floor(_e49);
    vec2 _e51 = pp;
    pp = (_e51 - fp);
    int x_3 = int(fp.x);
    int y_3 = int(fp.y);
    vec3 _e59 = scene_texel(x_3, (y_3 - 1));
    vec3 _e64 = scene_texel((x_3 + 1), (y_3 - 1));
    vec3 _e67 = scene_texel((x_3 - 1), y_3);
    vec3 _e68 = scene_texel(x_3, y_3);
    vec3 _e71 = scene_texel((x_3 + 1), y_3);
    vec3 _e74 = scene_texel((x_3 + 2), y_3);
    vec3 _e79 = scene_texel((x_3 - 1), (y_3 + 1));
    vec3 _e82 = scene_texel(x_3, (y_3 + 1));
    vec3 _e87 = scene_texel((x_3 + 1), (y_3 + 1));
    vec3 _e92 = scene_texel((x_3 + 2), (y_3 + 1));
    vec3 _e95 = scene_texel(x_3, (y_3 + 2));
    vec3 _e100 = scene_texel((x_3 + 1), (y_3 + 2));
    float _e101 = easu_luma(_e59);
    float _e102 = easu_luma(_e64);
    float _e103 = easu_luma(_e67);
    float _e104 = easu_luma(_e68);
    float _e105 = easu_luma(_e71);
    float _e106 = easu_luma(_e74);
    float _e107 = easu_luma(_e79);
    float _e108 = easu_luma(_e82);
    float _e109 = easu_luma(_e87);
    float _e110 = easu_luma(_e92);
    float _e111 = easu_luma(_e95);
    float _e112 = easu_luma(_e100);
    float _e119 = pp.x;
    float _e123 = pp.y;
    easu_set(((1.0 - _e119) * (1.0 - _e123)), _e101, _e103, _e104, _e105, _e108, dir, len);
    float _e128 = pp.x;
    float _e130 = pp.y;
    easu_set((_e128 * (1.0 - _e130)), _e102, _e104, _e105, _e106, _e109, dir, len);
    float _e135 = pp.x;
    float _e139 = pp.y;
    easu_set(((1.0 - _e135) * _e139), _e104, _e107, _e108, _e109, _e111, dir, len);
    float _e142 = pp.x;
    float _e144 = pp.y;
    easu_set((_e142 * _e144), _e105, _e108, _e109, _e110, _e112, dir, len);
    vec2 _e146 = dir;
    vec2 _e147 = dir;
    vec2 dir2_ = (_e146 * _e147);
    dirr = (dir2_.x + dir2_.y);
    float _e153 = dirr;
    bool zero = (_e153 < 3.0517578e-5);
    float _e156 = dirr;
    dirr = (zero ? 1.0 : inversesqrt(max(_e156, 1e-30)));
    float _e164 = dir.x;
    dir.x = (zero ? 1.0 : _e164);
    vec2 _e167 = dir;
    float _e168 = dirr;
    dir = (_e167 * _e168);
    float _e170 = len;
    len = (_e170 * 0.5);
    float _e173 = len;
    float _e174 = len;
    len = (_e173 * _e174);
    float _e177 = dir.x;
    float _e179 = dir.x;
    float _e182 = dir.y;
    float _e184 = dir.y;
    float _e188 = dir.x;
    float _e191 = dir.y;
    float stretch = (((_e177 * _e179) + (_e182 * _e184)) / max(abs(_e188), abs(_e191)));
    float _e197 = len;
    float _e201 = len;
    vec2 len2_1 = vec2((1.0 + ((stretch - 1.0) * _e197)), (1.0 - (0.5 * _e201)));
    float _e207 = len;
    float lob_1 = (0.5 + (-0.29 * _e207));
    float clp_1 = (1.0 / lob_1);
    vec2 _e220 = pp;
    vec2 _e222 = dir;
    easu_tap((vec2(0.0, -1.0) - _e220), _e222, len2_1, lob_1, clp_1, _e59, acc);
    vec2 _e226 = pp;
    vec2 _e228 = dir;
    easu_tap((vec2(1.0, -1.0) - _e226), _e228, len2_1, lob_1, clp_1, _e64, acc);
    vec2 _e232 = pp;
    vec2 _e234 = dir;
    easu_tap((vec2(-1.0, 1.0) - _e232), _e234, len2_1, lob_1, clp_1, _e79, acc);
    vec2 _e238 = pp;
    vec2 _e240 = dir;
    easu_tap((vec2(0.0, 1.0) - _e238), _e240, len2_1, lob_1, clp_1, _e82, acc);
    vec2 _e244 = pp;
    vec2 _e246 = dir;
    easu_tap((vec2(0.0, 0.0) - _e244), _e246, len2_1, lob_1, clp_1, _e68, acc);
    vec2 _e250 = pp;
    vec2 _e252 = dir;
    easu_tap((vec2(-1.0, 0.0) - _e250), _e252, len2_1, lob_1, clp_1, _e67, acc);
    vec2 _e256 = pp;
    vec2 _e258 = dir;
    easu_tap((vec2(1.0, 1.0) - _e256), _e258, len2_1, lob_1, clp_1, _e87, acc);
    vec2 _e262 = pp;
    vec2 _e264 = dir;
    easu_tap((vec2(2.0, 1.0) - _e262), _e264, len2_1, lob_1, clp_1, _e92, acc);
    vec2 _e268 = pp;
    vec2 _e270 = dir;
    easu_tap((vec2(2.0, 0.0) - _e268), _e270, len2_1, lob_1, clp_1, _e74, acc);
    vec2 _e274 = pp;
    vec2 _e276 = dir;
    easu_tap((vec2(1.0, 0.0) - _e274), _e276, len2_1, lob_1, clp_1, _e71, acc);
    vec2 _e280 = pp;
    vec2 _e282 = dir;
    easu_tap((vec2(1.0, 2.0) - _e280), _e282, len2_1, lob_1, clp_1, _e100, acc);
    vec2 _e286 = pp;
    vec2 _e288 = dir;
    easu_tap((vec2(0.0, 2.0) - _e286), _e288, len2_1, lob_1, clp_1, _e95, acc);
    vec3 lo = min(min(_e68, _e71), min(_e82, _e87));
    vec3 hi = max(max(_e68, _e71), max(_e82, _e87));
    vec4 _e295 = acc;
    float _e298 = acc.w;
    vec3 pix = clamp((_e295.xyz / vec3(_e298)), lo, hi);
    _fs2p_location0 = vec4(clamp(pix, vec3(0.0), vec3(1.0)), 1.0);
    return;
}


void main() { main_body_(); }
