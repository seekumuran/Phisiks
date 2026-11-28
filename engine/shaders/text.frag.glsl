#version 460 core
// Built-in bitmap font: one texel per font pixel (nearest).
layout(binding = 0) uniform sampler2D u_font;
in vec2 v_texel;
in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main() {
    if (texelFetch(u_font, ivec2(floor(v_texel)), 0).r < 0.5) discard;
    o_color = v_color;
}
