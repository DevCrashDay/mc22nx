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
        vec2 d_1 = (fract(t) - vec2(0.5));
        vec2 region = (vec2(0.5) - (vec2(0.5) / scale));
        p = (floor(t) + ((d_1 - clamp(d_1, -(region), region)) * scale));
    }
    vec2 _e53 = p;
    vec2 origin = floor(_e53);
    vec2 _e55 = p;
    vec2 f_1 = (_e55 - origin);
    int x_2 = int(origin.x);
    int y_2 = int(origin.y);
    vec4 _e61 = texel(base_1, x_2, y_2);
    vec4 _e64 = texel(base_1, (x_2 + 1), y_2);
    vec4 top = mix(_e61, _e64, f_1.x);
    vec4 _e69 = texel(base_1, x_2, (y_2 + 1));
    vec4 _e74 = texel(base_1, (x_2 + 1), (y_2 + 1));
    vec4 bottom = mix(_e69, _e74, f_1.x);
    return mix(top, bottom, f_1.y);
}

vec3 scene(vec2 uv_1) {
    vec3 c_1 = vec3(0.0);
    uint _e3 = _group_0_binding_1_fs.current;
    vec4 _e4 = sample_(_e3, uv_1);
    c_1 = _e4.xyz;
    uint _e9 = _group_0_binding_1_fs.blend;
    if ((_e9 != 0u)) {
        vec3 _e12 = c_1;
        uint _e15 = _group_0_binding_1_fs.previous;
        vec4 _e16 = sample_(_e15, uv_1);
        c_1 = ((_e12 + _e16.xyz) * 0.5);
    }
    vec3 _e21 = c_1;
    return _e21;
}

vec4 catmull_rom(float f) {
    float f2_ = (f * f);
    float f3_ = (f2_ * f);
    return vec4((((-0.5 * f3_) + f2_) - (0.5 * f)), (((1.5 * f3_) - (2.5 * f2_)) + 1.0), (((-1.5 * f3_) + (2.0 * f2_)) + (0.5 * f)), ((0.5 * f3_) - (0.5 * f2_)));
}

vec4 linear_weights(float u) {
    float b_1 = floor(u);
    float t_1 = (u - b_1);
    if ((b_1 < 0.0)) {
        return vec4((1.0 - t_1), t_1, 0.0, 0.0);
    }
    if ((b_1 < 1.0)) {
        return vec4(0.0, (1.0 - t_1), t_1, 0.0);
    }
    return vec4(0.0, 0.0, (1.0 - t_1), t_1);
}

vec3 row4_(vec3 a, vec3 b, vec3 c_2, vec3 d, vec4 w) {
    return ((((a * w.x) + (b * w.y)) + (c_2 * w.z)) + (d * w.w));
}

vec3 scene_texel(int x_1, int y_1) {
    vec3 c_3 = vec3(0.0);
    uint _e4 = _group_0_binding_1_fs.current;
    vec4 _e5 = texel(_e4, x_1, y_1);
    c_3 = _e5.xyz;
    uint _e10 = _group_0_binding_1_fs.blend;
    if ((_e10 != 0u)) {
        vec3 _e13 = c_3;
        uint _e16 = _group_0_binding_1_fs.previous;
        vec4 _e17 = texel(_e16, x_1, y_1);
        c_3 = ((_e13 + _e17.xyz) * 0.5);
    }
    vec3 _e22 = c_3;
    return _e22;
}

vec3 sharp_scene(vec2 uv_2) {
    uint _e3 = _group_0_binding_1_fs.width;
    uint _e7 = _group_0_binding_1_fs.height;
    vec2 size_1 = vec2(float(_e3), float(_e7));
    vec2 p_1 = ((uv_2 * size_1) - vec2(0.5));
    vec2 origin_1 = floor(p_1);
    vec2 f_2 = (p_1 - origin_1);
    int x_3 = int(origin_1.x);
    int y_3 = int(origin_1.y);
    vec3 _e24 = scene_texel((x_3 + -1), (y_3 + -1));
    vec3 _e29 = scene_texel((x_3 + 0), (y_3 + -1));
    vec3 _e34 = scene_texel((x_3 + 1), (y_3 + -1));
    vec3 _e39 = scene_texel((x_3 + 2), (y_3 + -1));
    vec3 _e44 = scene_texel((x_3 + -1), (y_3 + 0));
    vec3 _e49 = scene_texel((x_3 + 0), (y_3 + 0));
    vec3 _e54 = scene_texel((x_3 + 1), (y_3 + 0));
    vec3 _e59 = scene_texel((x_3 + 2), (y_3 + 0));
    vec3 _e64 = scene_texel((x_3 + -1), (y_3 + 1));
    vec3 _e69 = scene_texel((x_3 + 0), (y_3 + 1));
    vec3 _e74 = scene_texel((x_3 + 1), (y_3 + 1));
    vec3 _e79 = scene_texel((x_3 + 2), (y_3 + 1));
    vec3 _e84 = scene_texel((x_3 + -1), (y_3 + 2));
    vec3 _e89 = scene_texel((x_3 + 0), (y_3 + 2));
    vec3 _e94 = scene_texel((x_3 + 1), (y_3 + 2));
    vec3 _e99 = scene_texel((x_3 + 2), (y_3 + 2));
    uint _e102 = _group_0_binding_1_fs.out_width;
    uint _e106 = _group_0_binding_1_fs.out_height;
    vec2 d_2 = clamp((size_1 / vec2(float(_e102), float(_e106))), vec2(0.0), vec2(0.99));
    vec4 _e116 = catmull_rom(f_2.x);
    vec4 _e118 = catmull_rom(f_2.y);
    vec4 _e120 = linear_weights(f_2.x);
    vec4 _e122 = linear_weights(f_2.y);
    vec4 _e126 = linear_weights((f_2.x - d_2.x));
    vec4 _e130 = linear_weights((f_2.x + d_2.x));
    vec4 _e134 = linear_weights((f_2.y - d_2.y));
    vec4 _e138 = linear_weights((f_2.y + d_2.y));
    vec3 _e139 = row4_(_e24, _e29, _e34, _e39, _e116);
    vec3 _e142 = row4_(_e44, _e49, _e54, _e59, _e116);
    vec3 _e146 = row4_(_e64, _e69, _e74, _e79, _e116);
    vec3 _e150 = row4_(_e84, _e89, _e94, _e99, _e116);
    vec3 c_5 = ((((_e139 * _e118.x) + (_e142 * _e118.y)) + (_e146 * _e118.z)) + (_e150 * _e118.w));
    vec3 _e154 = row4_(_e24, _e29, _e34, _e39, _e120);
    vec3 _e157 = row4_(_e44, _e49, _e54, _e59, _e120);
    vec3 _e161 = row4_(_e64, _e69, _e74, _e79, _e120);
    vec3 _e165 = row4_(_e84, _e89, _e94, _e99, _e120);
    vec3 n = ((((_e154 * _e134.x) + (_e157 * _e134.y)) + (_e161 * _e134.z)) + (_e165 * _e134.w));
    vec3 _e169 = row4_(_e24, _e29, _e34, _e39, _e120);
    vec3 _e172 = row4_(_e44, _e49, _e54, _e59, _e120);
    vec3 _e176 = row4_(_e64, _e69, _e74, _e79, _e120);
    vec3 _e180 = row4_(_e84, _e89, _e94, _e99, _e120);
    vec3 s = ((((_e169 * _e138.x) + (_e172 * _e138.y)) + (_e176 * _e138.z)) + (_e180 * _e138.w));
    vec3 _e184 = row4_(_e24, _e29, _e34, _e39, _e126);
    vec3 _e187 = row4_(_e44, _e49, _e54, _e59, _e126);
    vec3 _e191 = row4_(_e64, _e69, _e74, _e79, _e126);
    vec3 _e195 = row4_(_e84, _e89, _e94, _e99, _e126);
    vec3 w_1 = ((((_e184 * _e122.x) + (_e187 * _e122.y)) + (_e191 * _e122.z)) + (_e195 * _e122.w));
    vec3 _e199 = row4_(_e24, _e29, _e34, _e39, _e130);
    vec3 _e202 = row4_(_e44, _e49, _e54, _e59, _e130);
    vec3 _e206 = row4_(_e64, _e69, _e74, _e79, _e130);
    vec3 _e210 = row4_(_e84, _e89, _e94, _e99, _e130);
    vec3 e = ((((_e199 * _e122.x) + (_e202 * _e122.y)) + (_e206 * _e122.z)) + (_e210 * _e122.w));
    vec3 mn = min(min(min(n, s), min(w_1, e)), c_5);
    vec3 mx = max(max(max(n, s), max(w_1, e)), c_5);
    vec3 amp = sqrt(clamp((min(mn, (vec3(1.0) - mx)) / max(mx, vec3(0.0001))), vec3(0.0), vec3(1.0)));
    vec3 k = (-(amp) * SHARPNESS);
    return ((c_5 + ((((n + s) + w_1) + e) * k)) / (vec3(1.0) + (4.0 * k)));
}

float luma(vec3 c_4) {
    return dot(c_4, vec3(0.299, 0.587, 0.114));
}

vec3 edge_aa(vec2 uv_3, vec3 center) {
    vec2 direction = vec2(0.0);
    bool local = false;
    uint _e4 = _group_0_binding_1_fs.width;
    uint _e8 = _group_0_binding_1_fs.height;
    vec2 texel_step = (vec2(1.0) / vec2(float(_e4), float(_e8)));
    uint _e16 = _group_0_binding_1_fs.width;
    uint _e20 = _group_0_binding_1_fs.height;
    ivec2 c_6 = ivec2(floor((uv_3 * vec2(float(_e16), float(_e20)))));
    vec3 _e32 = scene_texel((c_6.x - 1), (c_6.y - 1));
    float _e33 = luma(_e32);
    vec3 _e40 = scene_texel((c_6.x + 1), (c_6.y - 1));
    float _e41 = luma(_e40);
    vec3 _e48 = scene_texel((c_6.x - 1), (c_6.y + 1));
    float _e49 = luma(_e48);
    vec3 _e56 = scene_texel((c_6.x + 1), (c_6.y + 1));
    float _e57 = luma(_e56);
    vec3 _e60 = scene_texel(c_6.x, c_6.y);
    float _e61 = luma(_e60);
    float low = min(_e61, min(min(_e33, _e41), min(_e49, _e57)));
    float high = max(_e61, max(max(_e33, _e41), max(_e49, _e57)));
    if (((high - low) < max(0.0312, (high * 0.125)))) {
        return center;
    }
    direction = vec2(-(((_e33 + _e41) - (_e49 + _e57))), ((_e33 + _e49) - (_e41 + _e57)));
    float reduce = max(((((_e33 + _e41) + _e49) + _e57) * 0.03125), 0.0078125);
    float _e93 = direction.x;
    float ax = abs(_e93);
    float _e96 = direction.y;
    float ay = abs(_e96);
    if ((min(ax, ay) < max((max(ax, ay) * EDGE_SLANT), reduce))) {
        return center;
    }
    vec2 _e104 = direction;
    direction = (clamp((_e104 / vec2((min(ax, ay) + reduce))), vec2(-6.0), vec2(6.0)) * texel_step);
    vec3 _e115 = scene(uv_3);
    vec2 _e116 = direction;
    vec3 _e121 = scene((uv_3 - (_e116 / vec2(6.0))));
    vec2 _e122 = direction;
    vec3 _e127 = scene((uv_3 + (_e122 / vec2(6.0))));
    vec3 a_1 = (0.5 * (_e121 + _e127));
    vec2 _e133 = direction;
    vec3 _e137 = scene((uv_3 - (_e133 * 0.5)));
    vec2 _e138 = direction;
    vec3 _e142 = scene((uv_3 + (_e138 * 0.5)));
    vec3 b_2 = ((a_1 * 0.5) + (0.25 * (_e137 + _e142)));
    float _e147 = luma(b_2);
    if (!((_e147 < low))) {
        local = (_e147 > high);
    } else {
        local = true;
    }
    bool _e154 = local;
    return (center + ((_e154 ? a_1 : b_2) - _e115));
}

void main_body_() {
    VertexOut in_ = VertexOut(gl_FragCoord, _vs2fs_location0);
    vec3 c = vec3(0.0);
    uint _e4 = _group_0_binding_1_fs.options;
    if (((_e4 & 4u) != 0u)) {
        vec3 _e10 = sharp_scene(in_.uv);
        c = _e10;
    } else {
        vec3 _e12 = scene(in_.uv);
        c = _e12;
    }
    uint _e15 = _group_0_binding_1_fs.options;
    if (((_e15 & 1u) != 0u)) {
        vec3 _e21 = c;
        vec3 _e22 = edge_aa(in_.uv, _e21);
        c = _e22;
    }
    vec3 _e23 = c;
    _fs2p_location0 = vec4(clamp(_e23, vec3(0.0), vec3(1.0)), 1.0);
    return;
}


void main() { main_body_(); }
