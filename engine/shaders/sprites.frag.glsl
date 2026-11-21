#version 460 core
// Shaded sphere impostor: point sprite -> unit disc -> view-space sphere normal, lit by the ambient
// hemisphere and the scene's directional lights, coloured by speed or by the relativistic Doppler
// shift of the line-of-sight velocity (doppler.glsl), plus an optional emissive glow.
//   FLUID_DEPTH  screen-space fluid pass 1: the sphere's linear view depth (R32F) and its view
//                normal + speed, depth-tested against the resolved scene depth.
#include "common.glsl"
#include "fog.glsl"
#include "doppler.glsl"

in vec3 v_center;
in float v_radius;
in float v_speed;
in vec3 v_velocity;
uniform vec3 u_color;
uniform vec3 u_fastColor;
uniform float u_fastSpeed;
uniform uint u_entityId;
uniform int u_colorMode;        // 0 speed, 1 Doppler
uniform float u_glow;
uniform float u_dopplerSpeed;

#ifdef FLUID_DEPTH
layout(binding = 0) uniform sampler2D u_sceneDepth;   // resolved scene depth (window target)
uniform float u_fluidScale;                           // scene pixels per fluid-buffer pixel (1 or 2)
layout(location = 0) out vec4 o_depth;                // r: linear view depth (m)
layout(location = 1) out vec4 o_normalSpeed;          // view-space sphere normal, speed (m/s)

void main() {
    vec2 c = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(c, c);
    if (r2 > 1.0) discard;
    vec3 viewNormal = vec3(c.x, -c.y, sqrt(1.0 - r2));
    vec3 P = (u_view * vec4(v_center, 1.0)).xyz + viewNormal * v_radius;
    vec4 clip = u_projection * vec4(P, 1.0);
    float depth = clip.z / clip.w * 0.5 + 0.5;
    if (depth > texelFetch(u_sceneDepth, ivec2(gl_FragCoord.xy * max(u_fluidScale, 1.0)), 0).r) discard;
    gl_FragDepth = depth;
    o_depth = vec4(-P.z, 0.0, 0.0, 1.0);
    o_normalSpeed = vec4(viewNormal, v_speed);
}
#else
layout(location = 0) out vec4 o_color;
layout(location = 1) out uint o_entity;

void main() {
    vec2 c = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(c, c);
    if (r2 > 1.0) discard;
    vec3 viewNormal = vec3(c.x, -c.y, sqrt(1.0 - r2));
    vec3 N = normalize(transpose(mat3(u_view)) * viewNormal);
    vec3 toPoint = v_center - u_cameraPosition.xyz;
    float distance = length(toPoint);
    vec3 albedo = mix(u_color, u_fastColor, clamp(v_speed / max(u_fastSpeed, 1e-3), 0.0, 1.0));
    if (u_colorMode == 1) albedo = DopplerShift(u_color, v_velocity, -toPoint / max(distance, 1e-4), u_dopplerSpeed);
    vec3 color = albedo * mix(u_ambientDown.rgb, u_ambientUp.rgb, N.y * 0.5 + 0.5) * u_skyZenith.w;
    int lightCount = int(u_groundColor.w + 0.5);
    for (int i = 0; i < lightCount; ++i) {
        Light light = LoadLight(uint(i));
        if (int(light.directionType.w + 0.5) != 0) continue;
        vec3 L = -normalize(light.directionType.xyz);
        color += albedo / PI * max(dot(N, L), 0.0) * light.colorIntensity.rgb * light.colorIntensity.w;
    }
    color += albedo * u_glow * (0.55 + 0.45 * viewNormal.z);   // emissive core, softer rim
    color = mix(color, u_fogColor.rgb, FogAmount(u_cameraPosition.xyz, toPoint / max(distance, 1e-4), distance));
    o_color = vec4(color, 1.0);
    o_entity = u_entityId;
}
#endif
