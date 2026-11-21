#version 460 core
// Decal volume: the unit cube (-0.5 .. 0.5) of u_decalWorld, generated from gl_VertexID (36 vertices).
#include "common.glsl"

uniform mat4 u_decalWorld;

const vec3 CORNERS[8] = vec3[8](vec3(-0.5, -0.5, -0.5), vec3(0.5, -0.5, -0.5), vec3(0.5, 0.5, -0.5), vec3(-0.5, 0.5, -0.5),
                                vec3(-0.5, -0.5, 0.5), vec3(0.5, -0.5, 0.5), vec3(0.5, 0.5, 0.5), vec3(-0.5, 0.5, 0.5));
const int INDICES[36] = int[36](0, 2, 1, 0, 3, 2,  4, 5, 6, 4, 6, 7,  0, 1, 5, 0, 5, 4,  3, 7, 6, 3, 6, 2,  0, 4, 7, 0, 7, 3,  1, 2, 6, 1, 6, 5);

void main() {
    gl_Position = u_viewProjection * (u_decalWorld * vec4(CORNERS[INDICES[gl_VertexID]], 1.0));
}
