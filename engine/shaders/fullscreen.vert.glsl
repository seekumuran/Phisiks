#version 460 core
// One triangle covering the screen (no vertex buffer). SKY puts it on the far plane so it only
// fills pixels no geometry wrote.
out vec2 v_uv;

void main() {
    vec2 position = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0;
    v_uv = position * 0.5 + 0.5;
#ifdef SKY
    gl_Position = vec4(position, 1.0, 1.0);
#else
    gl_Position = vec4(position, 0.0, 1.0);
#endif
}
