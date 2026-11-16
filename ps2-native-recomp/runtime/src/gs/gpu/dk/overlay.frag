#version 430 core
layout(std430, binding = 0) readonly buffer Pix { uint pix[]; };
layout(std140, binding = 1) uniform Rect { uvec4 size; } rect;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;
void main() {
    uint w = max(rect.size.x, 1u), h = max(rect.size.y, 1u);
    uint x = min(uint(v_uv.x * float(w)), w - 1u);
    uint y = min(uint(v_uv.y * float(h)), h - 1u);
    uint c = pix[y * w + x];
    frag_color = vec4(float(c & 0xFFu), float((c >> 8) & 0xFFu), float((c >> 16) & 0xFFu), float((c >> 24) & 0xFFu)) / 255.0;
}
