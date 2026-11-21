#version 460 core
// Analytical SDF visualiser: sphere-traces the SDF/CSG scene (sdf.glsl) inside its bounding box,
// shades the surface with the PBR lighting, writes depth so it composes with meshes, and
// optionally shows a slice plane coloured by signed distance with 0.1 m isolines.
#include "common.glsl"
#include "brdf.glsl"
#include "fog.glsl"
#include "shadows.glsl"
#include "sdf.glsl"

in vec2 v_uv;
uniform vec3 u_boundsMin;
uniform vec3 u_boundsMax;
uniform vec4 u_surface;        // rgb albedo, a roughness
uniform float u_metallic;
uniform uint u_entityId;
uniform int u_showSlice;
uniform vec4 u_slicePlane;     // xyz unit normal, w offset (dot(n, p) = w)
layout(location = 0) out vec4 o_color;
layout(location = 1) out uint o_entity;

vec3 Normal(vec3 p) {
    const float e = 1e-3;
    return normalize(vec3(SceneSdf(p + vec3(e, 0, 0)) - SceneSdf(p - vec3(e, 0, 0)),
                          SceneSdf(p + vec3(0, e, 0)) - SceneSdf(p - vec3(0, e, 0)),
                          SceneSdf(p + vec3(0, 0, e)) - SceneSdf(p - vec3(0, 0, e))));
}

vec3 Shade(vec3 p, vec3 N, vec3 V) {
    vec3 albedo = u_surface.rgb;
    float roughness = clamp(u_surface.a, 0.04, 1.0);
    vec3 color = albedo * (1.0 - u_metallic) * mix(u_ambientDown.rgb, u_ambientUp.rgb, N.y * 0.5 + 0.5) * u_skyZenith.w;
    int lightCount = int(u_groundColor.w + 0.5);
    bool shadows = u_fogParams2.y > 0.5;
    for (int i = 0; i < lightCount; ++i) {
        Light light = LoadLight(uint(i));
        int type = int(light.directionType.w + 0.5);
        int shadowIndex = light.params.z < 0.0 ? -1 : int(light.params.z + 0.5);
        vec3 L;
        float attenuation = 1.0, shadow = 1.0;
        if (type == 0) {
            L = -normalize(light.directionType.xyz);
            if (shadows && shadowIndex >= 0) shadow = DirectionalShadow(p, N, L);
        }
        else {
            vec3 toLight = light.positionRange.xyz - p;
            float d = length(toLight);
            L = toLight / max(d, 1e-4);
            attenuation = DistanceAttenuation(d, light.positionRange.w);
            if (type == 2) attenuation *= SpotFactor(dot(-L, normalize(light.directionType.xyz)), light.params.x, light.params.y);
        }
        color += ShadeLight(N, V, L, albedo, roughness, u_metallic) * light.colorIntensity.rgb * light.colorIntensity.w * attenuation * shadow;
    }
    return color;
}

void main() {
    vec4 nearPoint = u_inverseViewProjection * vec4(v_uv * 2.0 - 1.0, -1.0, 1.0);
    vec4 farPoint = u_inverseViewProjection * vec4(v_uv * 2.0 - 1.0, 1.0, 1.0);
    vec3 ro = nearPoint.xyz / nearPoint.w;
    vec3 rd = normalize(farPoint.xyz / farPoint.w - ro);

    // Ray / bounding-box interval
    vec3 inv = 1.0 / rd;
    vec3 t0 = (u_boundsMin - ro) * inv, t1 = (u_boundsMax - ro) * inv;
    float tEnter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), max(min(t0.z, t1.z), 0.0));
    float tExit = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
    if (tEnter > tExit) discard;

    float t = tEnter;
    bool hit = false;
    for (int i = 0; i < 160 && t <= tExit; ++i) {
        float d = SceneSdf(ro + rd * t);
        if (d < 2e-4 + 1e-4 * t) { hit = true; break; }
        t += d;
    }

    float tSlice = 1e30;
    if (u_showSlice != 0) {
        float denom = dot(u_slicePlane.xyz, rd);
        if (abs(denom) > 1e-6) {
            float ts = (u_slicePlane.w - dot(u_slicePlane.xyz, ro)) / denom;
            if (ts >= tEnter && ts <= tExit && (!hit || ts < t)) tSlice = ts;
        }
    }
    if (!hit && tSlice > 1e29) discard;

    vec3 p;
    vec3 color;
    if (tSlice < 1e29) {
        p = ro + rd * tSlice;
        float d = SceneSdf(p);
        vec3 base = d < 0.0 ? vec3(0.15, 0.35, 0.95) : vec3(0.95, 0.55, 0.15);
        float bands = 0.55 + 0.45 * cos(d * 62.83);                       // 0.1 m distance bands
        float line = 1.0 - smoothstep(0.0, 1.5 * fwidth(d * 10.0), abs(fract(d * 10.0 + 0.5) - 0.5));
        float zero = 1.0 - smoothstep(0.0, 2.0 * fwidth(d), abs(d));
        color = base * bands * 0.6 + vec3(line * 0.25) + vec3(1.0) * zero;
    }
    else {
        p = ro + rd * t;
        color = Shade(p, Normal(p), -rd);
    }
    vec3 toPoint = p - u_cameraPosition.xyz;
    float distance = length(toPoint);
    color = mix(color, u_fogColor.rgb, FogAmount(u_cameraPosition.xyz, toPoint / max(distance, 1e-4), distance));

    vec4 clip = u_viewProjection * vec4(p, 1.0);
    gl_FragDepth = clip.z / clip.w * 0.5 + 0.5;
    o_color = vec4(color, 1.0);
    o_entity = u_entityId;
}
