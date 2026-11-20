#version 460 core
// Shaded sphere impostor: point sprite -> unit disc -> view-space sphere normal, lit by the ambient
// hemisphere and the scene's directional lights, coloured by speed.
#include "common.glsl"
#include "fog.glsl"

in vec3 v_center;
in float v_radius;
in float v_speed;
uniform vec3 u_color;
uniform vec3 u_fastColor;
uniform float u_fastSpeed;
uniform uint u_entityId;
layout(location = 0) out vec4 o_color;
layout(location = 1) out uint o_entity;

void main() {
    vec2 c = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(c, c);
    if (r2 > 1.0) discard;
    vec3 viewNormal = vec3(c.x, -c.y, sqrt(1.0 - r2));
    vec3 N = normalize(transpose(mat3(u_view)) * viewNormal);
    vec3 albedo = mix(u_color, u_fastColor, clamp(v_speed / max(u_fastSpeed, 1e-3), 0.0, 1.0));
    vec3 color = albedo * mix(u_ambientDown.rgb, u_ambientUp.rgb, N.y * 0.5 + 0.5) * u_skyZenith.w;
    int lightCount = int(u_groundColor.w + 0.5);
    for (int i = 0; i < lightCount; ++i) {
        if (int(u_lights[i].directionType.w + 0.5) != 0) continue;
        vec3 L = -normalize(u_lights[i].directionType.xyz);
        color += albedo / PI * max(dot(N, L), 0.0) * u_lights[i].colorIntensity.rgb * u_lights[i].colorIntensity.w;
    }
    vec3 toPoint = v_center - u_cameraPosition.xyz;
    float distance = length(toPoint);
    color = mix(color, u_fogColor.rgb, FogAmount(u_cameraPosition.xyz, toPoint / max(distance, 1e-4), distance));
    o_color = vec4(color, 1.0);
    o_entity = u_entityId;
}
