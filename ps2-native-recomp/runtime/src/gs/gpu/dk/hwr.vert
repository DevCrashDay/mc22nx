#version 460
// GPU triangle renderer (backend_deko3d.cpp hwr_draw): GS shapes in target pixels -> clip space.
// a_pos: x, y in target pixels (row 0 at the top), z = GS Z (raw), w = fog (0..1). state[1].x = 1 / Zmax as float bits.
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_col;   // RGBA8, unorm
layout(location = 2) in vec3 a_stq;   // s, t, q (texels with FST, else s, t, q as the GS has them)

layout(std140, binding = 0) uniform Draw {
    uvec4 state[4];
    vec4 size;  // target width, height
} u;

layout(location = 0) out vec4 v_col;
layout(location = 1) out vec3 v_stq;
layout(location = 2) out float v_fog;

void main() {
    const float zscale = uintBitsToFloat(u.state[1].x);
    const float z = clamp(a_pos.z * zscale, 0.0, 1.0);
    gl_Position = vec4(a_pos.x / u.size.x * 2.0 - 1.0, 1.0 - a_pos.y / u.size.y * 2.0, z * 2.0 - 1.0, 1.0);
    v_col = a_col;
    v_stq = a_stq;
    v_fog = a_pos.w;
}
