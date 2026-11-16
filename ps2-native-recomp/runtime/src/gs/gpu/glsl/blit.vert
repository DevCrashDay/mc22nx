#version 430 core
// glsl_compat.py: converted for nouveau and Ryujinx (see the script)
struct VertexOut {
    vec4 position;
    vec2 uv;
};
const float SHARPNESS = 0.14;
const float EDGE_SLANT = 0.04;
const float EDGE_REACH = 6.0;

layout(location = 0) smooth out vec2 _vs2fs_location0;

void main_body_() {
    uint i = uint(gl_VertexID);
    VertexOut o = VertexOut(vec4(0.0), vec2(0.0));
    vec2 p = vec2(float(((i << 1u) & 2u)), float((i & 2u)));
    o.position = vec4(((p * 2.0) - vec2(1.0)), 0.0, 1.0);
    o.uv = vec2(p.x, (1.0 - p.y));
    VertexOut _e26 = o;
    gl_Position = _e26.position;
    _vs2fs_location0 = _e26.uv;
    gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;  // glsl_compat.py: no y flip in OpenGL
    return;
}


void main() { main_body_(); }
