#version 460 core
// Analytic sky: horizon-to-zenith gradient, ground below the horizon, a sun disc and glow for
// every directional light, blended into the fog colour near the horizon so distant geometry and
// sky meet seamlessly.
#include "common.glsl"
#include "fog.glsl"

in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(location = 1) out uint o_entity;   // sky = no entity: written explicitly, see Renderer (sky pass)

void main() {
    o_entity = 0u;
    if (u_fogParams2.w > 0.5) {
        // Orthographic views (top/front/side): plain vertical backdrop gradient.
        o_color = vec4(mix(u_skyHorizon.rgb, u_skyZenith.rgb, v_uv.y), 1.0);
        return;
    }
    vec4 world = u_inverseViewProjection * vec4(v_uv * 2.0 - 1.0, 1.0, 1.0);
    vec3 dir = normalize(world.xyz / world.w - u_cameraPosition.xyz);

    float up = clamp(dir.y, 0.0, 1.0);
    vec3 sky = mix(u_skyHorizon.rgb, u_skyZenith.rgb, sqrt(up));
    sky = mix(sky, u_groundColor.rgb * 0.6, smoothstep(0.0, -0.15, dir.y));

    int lightCount = int(u_groundColor.w + 0.5);
    for (int i = 0; i < lightCount; ++i) {
        Light light = LoadLight(uint(i));
        if (int(light.directionType.w + 0.5) != 0) continue;
        vec3 toSun = -normalize(light.directionType.xyz);
        float c = max(dot(dir, toSun), 0.0);
        vec3 sun = light.colorIntensity.rgb * light.colorIntensity.w;
        sky += sun * (0.02 * pow(c, 8.0) + 6.0 * smoothstep(0.9995, 0.9998, c));
    }

    // Fog toward the horizon (as if the ray hit something ~150 m away), none overhead.
    float fog = FogAmount(u_cameraPosition.xyz, dir, 150.0) * (1.0 - smoothstep(0.0, 0.35, dir.y));
    o_color = vec4(mix(sky, u_fogColor.rgb, fog), 1.0);
}
