#version 460 core
// Screen-space fluid (SSF), passes 2-4 (pass 1 is sprites.frag.glsl with FLUID_DEPTH):
//   FLUID_BLUR       narrow-range bilateral Gaussian of the linear depth along u_direction: taps
//                    are clamped to +-u_falloff of the centre depth (no bleeding across silhouettes
//                    or between overlapping sheets), background taps are skipped
//   FLUID_NORMALS    view-space normals from the smoothed depth (smaller-difference side per axis)
//   FLUID_COMPOSITE  water shading over the lit scene + the breakdown wipe
#include "common.glsl"

uniform vec4 u_viewRect;          // pixels (x, y, w, h) inside the target (fluid resolution for blur / normals)
uniform float u_fluidScale;       // composite: target pixels per fluid-buffer pixel (1 or 2)
layout(location = 0) out vec4 o_color;

// View-space position of a pixel at linear depth d (perspective or orthographic projection).
vec3 ViewPosition(vec2 pixel, float d) {
    vec2 ndc = (pixel - u_viewRect.xy) / u_viewRect.zw * 2.0 - 1.0;
    if (u_fogParams2.w > 0.5) return vec3((ndc.x - u_projection[3][0]) / u_projection[0][0], (ndc.y - u_projection[3][1]) / u_projection[1][1], -d);
    return vec3((ndc.x + u_projection[2][0]) * d / u_projection[0][0], (ndc.y + u_projection[2][1]) * d / u_projection[1][1], -d);
}

#ifdef FLUID_BLUR
layout(binding = 0) uniform sampler2D u_depthIn;
uniform vec2 u_direction;
uniform float u_worldRadius;      // metres
uniform float u_pixelsPerMeter;   // at depth 1 (perspective) or absolute (orthographic)
uniform float u_maxRadius;
uniform float u_falloff;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float d = texelFetch(u_depthIn, p, 0).r;
    if (d <= 0.0) { o_color = vec4(0.0); return; }
    float radius = clamp(u_worldRadius * u_pixelsPerMeter / (u_fogParams2.w > 0.5 ? 1.0 : d), 1.0, u_maxRadius);
    // at most 12 taps per side: wide kernels (close to the camera) are sampled with a stride
    int n = min(int(ceil(radius)), 12);
    float stride = radius / float(n);
    float sigma = max(radius * 0.5, 0.5);
    float invTwoSigma2 = 1.0 / (2.0 * sigma * sigma);
    ivec2 lo = ivec2(u_viewRect.xy), hi = ivec2(u_viewRect.xy + u_viewRect.zw) - 1;
    float sum = 0.0, weight = 0.0;
    for (int i = -n; i <= n; ++i) {
        float offset = float(i) * stride;
        ivec2 q = clamp(p + ivec2(round(u_direction * offset)), lo, hi);
        float s = texelFetch(u_depthIn, q, 0).r;
        if (s <= 0.0) continue;
        s = clamp(s, d - u_falloff, d + u_falloff);
        float w = exp(-offset * offset * invTwoSigma2);
        sum += s * w;
        weight += w;
    }
    o_color = vec4(sum / max(weight, 1e-6), 0.0, 0.0, 1.0);
}
#endif

#ifdef FLUID_NORMALS
layout(binding = 0) uniform sampler2D u_depthIn;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float d = texelFetch(u_depthIn, p, 0).r;
    if (d <= 0.0) { o_color = vec4(0.0); return; }
    vec2 pixel = vec2(p) + 0.5;
    vec3 P = ViewPosition(pixel, d);
    float dr = texelFetch(u_depthIn, p + ivec2(1, 0), 0).r, dl = texelFetch(u_depthIn, p - ivec2(1, 0), 0).r;
    float du = texelFetch(u_depthIn, p + ivec2(0, 1), 0).r, dd = texelFetch(u_depthIn, p - ivec2(0, 1), 0).r;
    vec3 ddx = (dr > 0.0 && (dl <= 0.0 || abs(dr - d) < abs(d - dl))) ? ViewPosition(pixel + vec2(1.0, 0.0), dr) - P
             : (dl > 0.0 ? P - ViewPosition(pixel - vec2(1.0, 0.0), dl) : vec3(1.0, 0.0, 0.0));
    vec3 ddy = (du > 0.0 && (dd <= 0.0 || abs(du - d) < abs(d - dd))) ? ViewPosition(pixel + vec2(0.0, 1.0), du) - P
             : (dd > 0.0 ? P - ViewPosition(pixel - vec2(0.0, 1.0), dd) : vec3(0.0, 1.0, 0.0));
    vec3 N = normalize(cross(ddx, ddy));
    if (N.z < 0.0) N = -N;   // face the camera
    o_color = vec4(N, 1.0);
}
#endif

#ifdef FLUID_COMPOSITE
layout(binding = 0) uniform sampler2D u_scene;        // lit HDR scene (copy, before the fluid)
layout(binding = 1) uniform sampler2D u_depth;        // smoothed fluid depth
layout(binding = 2) uniform sampler2D u_normals;      // view-space normals
layout(binding = 3) uniform sampler2D u_raw;          // raw sphere normal + speed
layout(binding = 4) uniform sampler2D u_rawDepth;     // raw fluid depth
layout(binding = 5) uniform sampler2D u_sceneDepth;   // resolved scene depth (0..1)
uniform float u_breakdown;
uniform vec3 u_absorption;
uniform vec3 u_deepColor;
uniform float u_refraction;
uniform vec2 u_foam;              // speed where foam starts, intensity
uniform vec3 u_rawColor;
uniform vec2 u_depthRange;        // breakdown depth view: near, far (m)

vec3 SkyColor(vec3 dir) {
    if (dir.y < 0.0) return mix(u_skyHorizon.rgb, u_groundColor.rgb, clamp(-dir.y * 4.0, 0.0, 1.0));
    return mix(u_skyHorizon.rgb, u_skyZenith.rgb, pow(clamp(dir.y, 0.0, 1.0), 0.6));
}

float LinearSceneDepth(float z01) {
    float ndc = z01 * 2.0 - 1.0;
    if (u_fogParams2.w > 0.5) return (ndc - u_projection[3][2]) / -u_projection[2][2];
    return u_projection[3][2] / (ndc + u_projection[2][2]);
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);                       // full resolution (scene)
    ivec2 f = ivec2(gl_FragCoord.xy / max(u_fluidScale, 1.0));   // fluid buffers (half resolution by default)
    float fx = (gl_FragCoord.x - u_viewRect.x) / u_viewRect.z;
    float wipe = fract(u_breakdown);
    int stage = int(floor(u_breakdown + 1e-4));
    if (stage < 3 && fx < wipe) stage += 1;
    stage = min(stage, 3);
    if (u_breakdown < 3.0 && wipe > 0.001 && abs(fx - wipe) * u_viewRect.z < 1.5) { o_color = vec4(2.0, 1.6, 0.4, 1.0); return; }   // wipe line

    mat3 viewToWorld = transpose(mat3(u_view));
    float rawDepth = texelFetch(u_rawDepth, f, 0).r;
    vec4 raw = texelFetch(u_raw, f, 0);
    int lightCount = int(u_groundColor.w + 0.5);
    if (stage == 0) {   // raw particles: lit spheres
        if (rawDepth <= 0.0) discard;
        vec3 N = normalize(viewToWorld * raw.xyz);
        vec3 c = u_rawColor * mix(u_ambientDown.rgb, u_ambientUp.rgb, N.y * 0.5 + 0.5) * u_skyZenith.w;
        for (int i = 0; i < lightCount; ++i) {
            Light light = LoadLight(uint(i));
            if (int(light.directionType.w + 0.5) != 0) continue;
            c += u_rawColor / PI * max(dot(N, -normalize(light.directionType.xyz)), 0.0) * light.colorIntensity.rgb * light.colorIntensity.w;
        }
        o_color = vec4(c, 1.0);
        return;
    }
    float d = texelFetch(u_depth, f, 0).r;
    if (d <= 0.0) discard;
    vec3 Nv = normalize(texture(u_normals, gl_FragCoord.xy / (max(u_fluidScale, 1.0) * vec2(textureSize(u_normals, 0)))).xyz + vec3(0.0, 0.0, 1e-4));
    if (stage == 1) {   // smoothed depth, near = bright
        float t = clamp((d - u_depthRange.x) / max(u_depthRange.y - u_depthRange.x, 1e-3), 0.0, 1.0);
        o_color = vec4(vec3(mix(1.2, 0.08, t)) * vec3(0.85, 0.95, 1.0), 1.0);
        return;
    }
    if (stage == 2) { o_color = vec4(Nv * 0.5 + 0.5, 1.0); return; }

    vec3 viewPos = ViewPosition(vec2(p) + 0.5, d);
    vec3 worldPos = u_cameraPosition.xyz + viewToWorld * viewPos;
    vec3 N = normalize(viewToWorld * Nv);
    vec3 V = normalize(u_cameraPosition.xyz - worldPos);
    float sceneZ = texelFetch(u_sceneDepth, p, 0).r;
    float thickness = sceneZ >= 1.0 ? 4.0 : max(LinearSceneDepth(sceneZ) - d, 0.0);

    // refraction + Beer-Lambert absorption over the water depth
    vec2 targetSize = vec2(textureSize(u_scene, 0)) * max(u_fluidScale, 1.0);   // the copy is at the fluid resolution
    vec2 offset = Nv.xy * u_refraction * u_viewRect.w * min(thickness, 1.0);
    vec3 behind = texture(u_scene, (gl_FragCoord.xy - offset) / targetSize).rgb;
    vec3 transmittance = exp(-u_absorption * thickness);
    vec3 lightSum = mix(u_ambientDown.rgb, u_ambientUp.rgb, N.y * 0.5 + 0.5) * u_skyZenith.w;
    vec3 specular = vec3(0.0);
    for (int i = 0; i < lightCount; ++i) {
        Light light = LoadLight(uint(i));
        if (int(light.directionType.w + 0.5) != 0) continue;
        vec3 L = -normalize(light.directionType.xyz);
        vec3 radiance = light.colorIntensity.rgb * light.colorIntensity.w;
        lightSum += radiance * max(dot(N, L), 0.0) / PI;
        vec3 H = normalize(L + V);
        specular += radiance * pow(max(dot(N, H), 0.0), 380.0) * 6.0;
    }
    vec3 body = behind * transmittance + u_deepColor * lightSum * (1.0 - transmittance);
    // Fresnel (Schlick, water F0 = 0.02) reflection of the sky
    float cosV = max(dot(N, V), 0.0);
    float F = 0.02 + 0.98 * pow(1.0 - cosV, 5.0);
    vec3 reflected = SkyColor(reflect(-V, N)) * u_skyZenith.w;
    vec3 color = mix(body, reflected, F) + specular * F * 4.0;
    // whitewater foam from the speed of the front-most particles
    float foam = smoothstep(u_foam.x, u_foam.x * 2.0, raw.w) * (rawDepth > 0.0 ? 1.0 : 0.0);
    color = mix(color, vec3(u_foam.y) * (0.35 + lightSum), foam * 0.7);
    o_color = vec4(color, 1.0);
}
#endif
