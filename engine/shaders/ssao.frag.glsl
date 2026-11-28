#version 460 core
// Screen-space ambient occlusion from the depth buffer (normal-oriented hemisphere kernel).
//   SSAO_PASS  : occlusion of u_samples hemisphere points around the reconstructed view-space
//                position (normal from neighbouring depth), rotated per pixel by interleaved
//                gradient noise; range check fades occluders far behind.
//   SSAO_BLUR  : 4x4 depth-aware box blur of the noisy AO.
//   SSAO_APPLY : writes the AO for multiplicative blending into the lit HDR image.
#include "common.glsl"

layout(binding = 0) uniform sampler2D u_depth;      // scene depth (full resolution)
layout(binding = 1) uniform sampler2D u_ao;         // blur / apply input
uniform mat4 u_inverseProjection;
uniform mat4 u_projectionMatrix;
uniform vec4 u_viewRect;                            // full-resolution view rectangle (pixels)
uniform vec2 u_targetSize;                          // full-resolution target size
uniform float u_scale;                              // full pixels per AO pixel (1 or 2)
uniform vec4 u_params;                              // radius (m), intensity, bias (m), samples
layout(location = 0) out vec4 o_color;

vec3 ViewAt(vec2 pixel) {
    float depth = texture(u_depth, pixel / u_targetSize).r;
    vec2 ndc = (pixel - u_viewRect.xy) / u_viewRect.zw * 2.0 - 1.0;
    vec4 v = u_inverseProjection * vec4(ndc, depth * 2.0 - 1.0, 1.0);
    return v.xyz / v.w;
}

void main() {
    vec2 pixel = gl_FragCoord.xy * u_scale;
#ifdef SSAO_PASS
    float depth = texture(u_depth, pixel / u_targetSize).r;
    if (depth >= 1.0) { o_color = vec4(1.0); return; }
    vec3 P = ViewAt(pixel);
    vec3 r = ViewAt(pixel + vec2(u_scale, 0.0)) - P, l = P - ViewAt(pixel - vec2(u_scale, 0.0));
    vec3 t = ViewAt(pixel + vec2(0.0, u_scale)) - P, b = P - ViewAt(pixel - vec2(0.0, u_scale));
    vec3 N = normalize(cross(dot(r, r) < dot(l, l) ? r : l, dot(t, t) < dot(b, b) ? t : b));
    if (dot(N, P) > 0.0) N = -N;                    // face the camera (view space: camera at origin)
    float radius = u_params.x, bias = u_params.z;
    int samples = int(u_params.w + 0.5);
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    vec3 helper = abs(N.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(helper, N)), B = cross(N, T);
    float occlusion = 0.0;
    for (int i = 0; i < samples; ++i) {
        // golden-angle spiral over the hemisphere, rotated per pixel; denser near the centre
        float fi = (float(i) + 0.5) / float(samples);
        float angle = float(i) * 2.39996323 + noise * 6.2831853;
        float cosTheta = sqrt(1.0 - fi), sinTheta = sqrt(fi);
        vec3 dir = vec3(cos(angle) * sinTheta, sin(angle) * sinTheta, cosTheta);
        float scale = mix(0.15, 1.0, fi * fi);
        vec3 S = P + (T * dir.x + B * dir.y + N * dir.z) * radius * scale;
        vec4 clip = u_projectionMatrix * vec4(S, 1.0);
        vec2 ndc = clip.xy / clip.w;
        vec2 samplePixel = u_viewRect.xy + (ndc * 0.5 + 0.5) * u_viewRect.zw;
        if (any(lessThan(samplePixel, u_viewRect.xy)) || any(greaterThan(samplePixel, u_viewRect.xy + u_viewRect.zw))) continue;
        float sceneZ = ViewAt(samplePixel).z;
        float range = smoothstep(0.0, 1.0, radius / max(abs(P.z - sceneZ), 1e-4));
        occlusion += (sceneZ >= S.z + bias ? 1.0 : 0.0) * range;
    }
    float ao = 1.0 - occlusion / float(max(samples, 1));
    o_color = vec4(vec3(pow(clamp(ao, 0.0, 1.0), u_params.y)), 1.0);
#endif
#ifdef SSAO_BLUR
    vec2 aoSize = vec2(textureSize(u_ao, 0));
    float centerZ = ViewAt(pixel).z;
    float sum = 0.0, weight = 0.0;
    for (int y = -2; y < 2; ++y) {
        for (int x = -2; x < 2; ++x) {
            vec2 offset = vec2(float(x) + 0.5, float(y) + 0.5);
            vec2 aoUv = (gl_FragCoord.xy + offset) / aoSize;
            float z = ViewAt(pixel + offset * u_scale).z;
            float w = 1.0 / (1e-3 + abs(z - centerZ) * 8.0);
            sum += texture(u_ao, aoUv).r * w;
            weight += w;
        }
    }
    o_color = vec4(vec3(sum / max(weight, 1e-5)), 1.0);
#endif
#ifdef SSAO_APPLY
    vec2 aoUv = (gl_FragCoord.xy / u_scale) / vec2(textureSize(u_ao, 0));
    o_color = vec4(vec3(texture(u_ao, aoUv).r), 1.0);
#endif
}
