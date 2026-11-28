#version 460 core
// Projected decal (deferred style on the resolved depth): reconstructs the scene position under
// the pixel, keeps it if it lies inside the decal box, and shades a procedural pattern in the
// box's XZ plane. Blending is chosen per decal (multiply / additive / alpha).
#include "common.glsl"

layout(binding = 0) uniform sampler2D u_depth;
uniform mat4 u_decalInverse;
uniform vec3 u_decalUp;          // world +Y of the box (projection axis)
uniform vec4 u_decalColor;
uniform vec4 u_decalParams;      // x pattern, y blend (0 multiply, 1 additive, 2 alpha), z angle fade, w seed
uniform vec4 u_viewRect;         // view rectangle in target pixels
uniform vec2 u_targetSize;

layout(location = 0) out vec4 o_color;

float H(vec2 p) { vec3 q = fract(vec3(p.xyx) * 0.1031); q += dot(q, q.yzx + 33.33); return fract((q.x + q.y) * q.z); }
float N(vec2 p) {
    vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
    return mix(mix(H(i), H(i + vec2(1, 0)), u.x), mix(H(i + vec2(0, 1)), H(i + vec2(1, 1)), u.x), u.y);
}
float Fbm3(vec2 p) { return 0.5 * N(p) + 0.25 * N(p * 2.1 + 3.7) + 0.125 * N(p * 4.3 - 1.9); }

vec3 WorldAt(vec2 pixel, float depth) {
    vec2 ndc = (pixel - u_viewRect.xy) / u_viewRect.zw * 2.0 - 1.0;
    vec4 w = u_inverseViewProjection * vec4(ndc, depth * 2.0 - 1.0, 1.0);
    return w.xyz / w.w;
}

void main() {
    vec2 pixel = gl_FragCoord.xy;
    float depth = texture(u_depth, pixel / u_targetSize).r;
    if (depth >= 1.0) discard;
    vec3 world = WorldAt(pixel, depth);
    vec3 local = (u_decalInverse * vec4(world, 1.0)).xyz;
    if (any(greaterThan(abs(local), vec3(0.5)))) discard;
    // surface normal from neighbouring depth (smaller difference side avoids silhouette smearing)
    vec2 texel = 1.0 / u_targetSize;
    vec3 r = WorldAt(pixel + vec2(1, 0), texture(u_depth, (pixel + vec2(1, 0)) * texel).r) - world;
    vec3 l = world - WorldAt(pixel - vec2(1, 0), texture(u_depth, (pixel - vec2(1, 0)) * texel).r);
    vec3 t = WorldAt(pixel + vec2(0, 1), texture(u_depth, (pixel + vec2(0, 1)) * texel).r) - world;
    vec3 b = world - WorldAt(pixel - vec2(0, 1), texture(u_depth, (pixel - vec2(0, 1)) * texel).r);
    vec3 dx = dot(r, r) < dot(l, l) ? r : l;
    vec3 dy = dot(t, t) < dot(b, b) ? t : b;
    vec3 n = normalize(cross(dx, dy));
    if (dot(n, u_decalUp) < 0.0) n = -n;
    float facing = smoothstep(u_decalParams.z, min(u_decalParams.z + 0.3, 1.0), dot(n, u_decalUp));
    float depthFade = 1.0 - smoothstep(0.3, 0.5, abs(local.y));

    vec2 uv = local.xz + 0.5;                       // 0..1 across the box
    vec2 c = uv - 0.5;
    float radius = length(c) * 2.0;
    float seed = u_decalParams.w * 17.0;
    int pattern = int(u_decalParams.x + 0.5);
    float alpha = 0.0;
    vec3 color = u_decalColor.rgb;
    if (pattern == 0) {                             // scorch: noisy radial burn, darkest in the middle
        float n1 = Fbm3(uv * 7.0 + seed);
        alpha = 1.0 - smoothstep(0.25, 1.0, radius + (n1 - 0.5) * 0.55);
        color = mix(u_decalColor.rgb, u_decalColor.rgb * 0.35, 1.0 - smoothstep(0.0, 0.6, radius));
    }
    else if (pattern == 1) {                        // glowing rings, slowly pulsing
        float pulse = 0.75 + 0.25 * sin(u_cameraPosition.w * 3.0 + seed);
        alpha = exp(-pow((radius - 0.78) / 0.05, 2.0)) + 0.6 * exp(-pow((radius - 0.5) / 0.025, 2.0)) * pulse
              + 0.25 * (1.0 - smoothstep(0.0, 0.35, radius)) * pulse;
    }
    else if (pattern == 2) {                        // paint splat with droplets
        float a = atan(c.y, c.x);
        float lobes = 0.55 + 0.12 * sin(a * 7.0 + seed) + 0.08 * sin(a * 13.0 - seed * 2.0);
        float blob = 1.0 - smoothstep(lobes - 0.04, lobes + 0.04, radius + (N(uv * 9.0 + seed) - 0.5) * 0.12);
        vec2 cell = floor(uv * 6.0);
        float drop = H(cell + seed) > 0.8 ? 1.0 - smoothstep(0.12, 0.2, length(fract(uv * 6.0) - 0.5)) : 0.0;
        alpha = max(blob, drop * step(radius, 0.95));
    }
    else {                                          // hazard: yellow / black stripes inside a border
        vec2 e = abs(c);
        float inside = step(max(e.x, e.y), 0.46);
        float stripe = step(0.5, fract((uv.x + uv.y) * 4.0));
        color = mix(vec3(0.02), u_decalColor.rgb, stripe);
        alpha = inside;
    }
    alpha *= u_decalColor.a * facing * depthFade;
    if (alpha <= 0.002) discard;
    int blend = int(u_decalParams.y + 0.5);
    if (blend == 0) o_color = vec4(mix(vec3(1.0), color, alpha), 1.0);         // DST_COLOR * src
    else if (blend == 1) o_color = vec4(color * alpha, 0.0);                    // ONE, ONE
    else o_color = vec4(color, alpha);                                          // SRC_ALPHA, ONE_MINUS_SRC_ALPHA
}
