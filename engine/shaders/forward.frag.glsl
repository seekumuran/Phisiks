#version 460 core
// Forward PBR shading: hemisphere ambient + directional/point/spot lights with shadows,
// procedural materials, analytic fog. Writes HDR colour and the entity id.
#include "common.glsl"
#include "brdf.glsl"
#include "fog.glsl"
#include "shadows.glsl"
#include "patterns.glsl"

in vec3 v_worldPos;
in vec3 v_normal;
in vec3 v_patternPos;
in vec3 v_objectNormal;
in vec2 v_uv;
flat in uint v_instance;

layout(location = 0) out vec4 o_color;
layout(location = 1) out uint o_entity;

void main() {
    Instance inst = LoadInstance(v_instance);
    Material m = LoadMaterial(uint(inst.scaleMaterial.w + 0.5));

    vec3 N = normalize(v_normal);
    if (!gl_FrontFacing) N = -N;

    float glow;
    vec4 surface = SamplePattern(m, v_patternPos, normalize(v_objectNormal), inst.uvScale.xy, glow);
    vec3 albedo = surface.rgb * SampleAlbedoTexture(m, v_patternPos, normalize(v_objectNormal), inst.uvScale.xy);
    float patternRoughness = m.params.z >= 0.0 ? m.params.z : m.baseRoughness.a;
    float roughness = clamp(mix(m.baseRoughness.a, patternRoughness, surface.a), 0.04, 1.0);
    float metallic = m.patternMetallic.a;

    vec3 V = normalize(u_cameraPosition.xyz - v_worldPos);
    float NoV = max(dot(N, V), 1e-4);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);

    // Ambient: studio hemisphere (up/down colours) for diffuse and, along the reflection, specular.
    float ambientIntensity = u_skyZenith.w;
    vec3 diffuseAmbient = mix(u_ambientDown.rgb, u_ambientUp.rgb, N.y * 0.5 + 0.5) * ambientIntensity;
    vec3 R = reflect(-V, N);
    vec3 specularAmbient = mix(u_ambientDown.rgb, u_ambientUp.rgb, smoothstep(-0.2, 0.2, R.y)) * ambientIntensity;
    vec3 fresnelAmbient = f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - NoV, 5.0);
    vec3 color = (1.0 - metallic) * albedo * diffuseAmbient * (vec3(1.0) - fresnelAmbient)
               + specularAmbient * fresnelAmbient * (1.0 - 0.7 * roughness);

    int lightCount = int(u_groundColor.w + 0.5);
    bool shadows = u_fogParams2.y > 0.5;
    for (int i = 0; i < lightCount; ++i) {
        Light light = LoadLight(uint(i));
        int type = int(light.directionType.w + 0.5);
        int shadowIndex = light.params.z < 0.0 ? -1 : int(light.params.z + 0.5);
        vec3 L;
        float attenuation = 1.0;
        float shadow = 1.0;
        if (type == 0) {
            L = -normalize(light.directionType.xyz);
            if (shadows && shadowIndex >= 0) shadow = DirectionalShadow(v_worldPos, N, L);
        }
        else {
            vec3 toLight = light.positionRange.xyz - v_worldPos;
            float d = length(toLight);
            L = toLight / max(d, 1e-4);
            attenuation = DistanceAttenuation(d, light.positionRange.w);
            if (type == 2) attenuation *= SpotFactor(dot(-L, normalize(light.directionType.xyz)), light.params.x, light.params.y);
            if (attenuation <= 0.0) continue;
            if (shadows && shadowIndex >= 0) {
                shadow = type == 2 ? SpotShadow(shadowIndex, v_worldPos, N)
                                   : PointShadow(shadowIndex, v_worldPos, N, light.positionRange.xyz, light.positionRange.w);
            }
        }
        color += ShadeLight(N, V, L, albedo, roughness, metallic) * light.colorIntensity.rgb * light.colorIntensity.w * attenuation * shadow;
    }
    // Rim light on vertical surfaces only (separates silhouettes from a dark backdrop).
    float rim = pow(1.0 - NoV, max(u_rim.w, 1.0)) * (1.0 - N.y * N.y);
    color += u_rim.rgb * rim * albedo * 2.0 + u_rim.rgb * rim * 0.15;
    color += m.emissivePattern.rgb + albedo * glow * m.params.w;

    vec3 toFragment = v_worldPos - u_cameraPosition.xyz;
    float distance = length(toFragment);
    float fog = FogAmount(u_cameraPosition.xyz, toFragment / max(distance, 1e-4), distance);
    color = mix(color, u_fogColor.rgb, fog);

    o_color = vec4(color, 1.0);
    o_entity = inst.ids.x;
}
