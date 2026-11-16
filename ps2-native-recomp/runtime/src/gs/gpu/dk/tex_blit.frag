#version 460
// present_image: a GPU renderer target to the screen (with dk/overlay.vert's full-screen triangle); shows the top-left
// region (region.xy = shown size / image size, region.zw = its top-left corner / image size). Single exit (see dk/hwr.frag).
layout(location = 0) in vec2 v_uv;
layout(std140, binding = 0) uniform Region { vec4 region; } u;
layout(binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 frag;

void main() {
    // vertical flip: the Switch screen showed the picture upside down with v_uv as is (the screen dump did not)
    frag = vec4(texture(tex, u.region.zw + vec2(v_uv.x, 1.0 - v_uv.y) * u.region.xy).rgb, 1.0);
}
