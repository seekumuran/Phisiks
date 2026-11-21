#version 460 core
// Camera-facing ribbons (trails, beams, orbit paths): each vertex is a centre point plus the path
// tangent; it is pushed sideways perpendicular to both the tangent and the view direction.
#include "common.glsl"

layout(location = 0) in vec4 a_positionSide;   // xyz centre, w side (-1 / +1)
layout(location = 1) in vec4 a_tangentWidth;   // xyz tangent, w width (m)
layout(location = 2) in vec4 a_color;          // linear HDR rgb, a opacity

out vec4 v_color;
out float v_side;

void main() {
    vec3 centre = a_positionSide.xyz;
    vec3 toCamera = u_fogParams2.w > 0.5 ? transpose(mat3(u_view))[2] : u_cameraPosition.xyz - centre;   // ortho: view axis
    vec3 side = cross(a_tangentWidth.xyz, toCamera);
    float len = length(side);
    side = len > 1e-6 ? side / len : vec3(0.0, 1.0, 0.0);
    vec3 world = centre + side * (a_positionSide.w * 0.5 * a_tangentWidth.w);
    v_color = a_color;
    v_side = a_positionSide.w;
    gl_Position = u_viewProjection * vec4(world, 1.0);
}
