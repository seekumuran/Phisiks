#version 460 core
// Shadow map pass. Directional/spot: depth only. Point lights: linear distance / range, so one
// cube-array comparison works for every face.
#include "common.glsl"

in vec3 v_worldPos;
#ifdef POINT_SHADOW
uniform vec4 u_lightPositionRange;
#endif

void main() {
#ifdef POINT_SHADOW
    gl_FragDepth = clamp(length(v_worldPos - u_lightPositionRange.xyz) / u_lightPositionRange.w, 0.0, 1.0);
#endif
}
