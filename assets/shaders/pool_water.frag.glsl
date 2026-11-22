#version 460 core
// Showcase 02 water surface (Render::PostPass). For every pixel whose view ray meets the pool's
// heightfield before the scene depth:
//   refraction   Snell's law (1 / 1.333) into the water; the refracted ray is traced against the
//                pool box (floor + walls) and the ball analytically, the hit is projected back to
//                the screen and the scene colour there is used, with the floor caustics applied and
//                Beer-Lambert absorption over the underwater path
//   reflection   reflected ray against the ball and a studio sky gradient + sun highlight
//   blend        Schlick Fresnel (f0 = 0.02)
// u_params[3].x `breakdown` 0 wireframe (scene only), 1 water depth, 2 normals, 3 final; a fractional
// value shows the next stage left of the wipe line.
//   u_params[0]  water level, half size, floor y, height texel size (m)
//   u_params[1]  xyz unit direction to the sun, w sun intensity
//   u_params[2]  ball centre xyz, radius
//   u_params[3]  x breakdown, y caustic strength, z refraction scale
//   u_params[4]  sky zenith rgb       u_params[5] sky horizon rgb
//   u_params[6]  absorption per metre rgb
//   u_params[7]  ball albedo rgb
// u_data0: height offsets (R32F, n x n over the pool), u_data1: floor caustics (R32F).
#include "common.glsl"

layout(binding = 0) uniform sampler2D u_sceneColor;
layout(binding = 1) uniform sampler2D u_sceneDepth;
layout(binding = 3) uniform sampler2D u_data0;
layout(binding = 4) uniform sampler2D u_data1;
uniform vec4 u_viewRect;
uniform vec2 u_targetSize;
uniform vec4 u_params[16];

in vec2 v_uv;
layout(location = 0) out vec4 o_color;

float LEVEL, HALF, FLOORY, TEXEL;

vec2 PoolUv(vec2 xz) { return (xz + HALF) / (2.0 * HALF); }
float Height(vec2 xz) { return texture(u_data0, PoolUv(xz)).r; }
vec3 SurfaceNormal(vec2 xz) {
    float e = TEXEL;
    float dx = Height(xz + vec2(e, 0)) - Height(xz - vec2(e, 0));
    float dz = Height(xz + vec2(0, e)) - Height(xz - vec2(0, e));
    return normalize(vec3(-dx, 2.0 * e, -dz));
}
float Caustic(vec3 p) {
    // floor map, looked up along the refracted sun direction for points above the floor
    vec3 L = normalize(u_params[1].xyz);
    vec2 xz = p.xz - L.xz / max(L.y, 0.2) * 0.75 * (p.y - FLOORY);
    return texture(u_data1, PoolUv(clamp(xz, vec2(-HALF), vec2(HALF)))).r;
}

vec3 Ndc(vec3 world) {
    vec4 c = u_viewProjection * vec4(world, 1.0);
    return c.xyz / c.w;
}
vec2 ToPixelUv(vec3 ndc) {
    vec2 px = u_viewRect.xy + (ndc.xy * 0.5 + 0.5) * u_viewRect.zw;
    return px / u_targetSize;
}

bool HitSphere(vec3 o, vec3 d, out float t) {
    vec3 oc = o - u_params[2].xyz;
    float b = dot(oc, d), c = dot(oc, oc) - u_params[2].w * u_params[2].w;
    float h = b * b - c;
    if (h < 0.0) return false;
    t = -b - sqrt(h);
    return t > 0.0;
}

vec3 Sky(vec3 d) {
    float t = clamp(d.y * 1.4, 0.0, 1.0);
    vec3 sky = mix(u_params[5].rgb, u_params[4].rgb, t);
    vec3 L = normalize(u_params[1].xyz);
    float s = pow(max(dot(d, L), 0.0), 900.0) * 60.0 + pow(max(dot(d, L), 0.0), 40.0) * 0.6;
    return sky + vec3(1.0, 0.97, 0.9) * s * u_params[1].w;
}

vec3 BallShade(vec3 p) {
    vec3 n = normalize(p - u_params[2].xyz);
    vec3 L = normalize(u_params[1].xyz);
    float diffuse = max(dot(n, L), 0.0) * u_params[1].w * 0.32 + 0.35;
    return u_params[7].rgb * diffuse;
}

void main() {
    LEVEL = u_params[0].x; HALF = u_params[0].y; FLOORY = u_params[0].z; TEXEL = u_params[0].w;
    ivec2 pix = ivec2(gl_FragCoord.xy);
    vec3 scene = texelFetch(u_sceneColor, pix, 0).rgb;
    float depth = texelFetch(u_sceneDepth, pix, 0).r;
    vec2 ndcXY = (gl_FragCoord.xy - u_viewRect.xy) / u_viewRect.zw * 2.0 - 1.0;
    vec4 nearH = u_inverseViewProjection * vec4(ndcXY, -1.0, 1.0);
    vec4 farH = u_inverseViewProjection * vec4(ndcXY, 1.0, 1.0);
    vec3 ro = nearH.xyz / nearH.w;
    vec3 rd = normalize(farH.xyz / farH.w - ro);
    vec4 sceneH = u_inverseViewProjection * vec4(ndcXY, depth * 2.0 - 1.0, 1.0);
    float sceneDist = depth >= 1.0 ? 1e9 : length(sceneH.xyz / sceneH.w - ro);

    float stage = clamp(u_params[3].x, 0.0, 3.0);
    float base = floor(stage);
    float wipe = (gl_FragCoord.x - u_viewRect.x) / u_viewRect.z;
    if (stage - base > 0.0 && wipe < stage - base) base += 1.0;
    if (base < 0.5 || rd.y > -1e-4) { o_color = vec4(scene, 1.0); return; }

    // view ray against the surface: plane at the rest level, refined on the heightfield
    float t = (LEVEL - ro.y) / rd.y;
    for (int k = 0; k < 4; ++k) {
        vec3 p = ro + rd * t;
        t = (LEVEL + Height(p.xz) - ro.y) / rd.y;
    }
    vec3 P = ro + rd * t;
    if (t <= 0.0 || t > sceneDist || abs(P.x) > HALF || abs(P.z) > HALF) { o_color = vec4(scene, 1.0); return; }
    vec3 N = SurfaceNormal(P.xz);

    // refracted path: pool box (floor and walls) or the ball
    vec3 R = refract(rd, N, 1.0 / 1.333);
    R = normalize(mix(rd, R, u_params[3].z));
    float tFloor = (FLOORY - P.y) / min(R.y, -1e-4);
    float tx = R.x > 0.0 ? (HALF - P.x) / R.x : (-HALF - P.x) / min(R.x, -1e-6);
    float tz = R.z > 0.0 ? (HALF - P.z) / R.z : (-HALF - P.z) / min(R.z, -1e-6);
    float tHit = min(tFloor, min(tx, tz));
    float tBall;
    bool ball = HitSphere(P, R, tBall) && tBall < tHit;
    if (ball) tHit = tBall;
    vec3 H = P + R * tHit;
    vec3 under;
    if (ball) under = BallShade(H);
    else {
        vec3 hn = Ndc(H);
        vec2 suv = clamp(ToPixelUv(hn), (u_viewRect.xy + 0.5) / u_targetSize, (u_viewRect.xy + u_viewRect.zw - 0.5) / u_targetSize);
        under = texture(u_sceneColor, suv).rgb;
        under *= mix(1.0, Caustic(H), u_params[3].y);
    }
    float path = tHit + max(LEVEL - H.y, 0.0) * 0.5;
    vec3 transmit = exp(-u_params[6].rgb * path);
    vec3 refracted = under * transmit + vec3(0.02, 0.09, 0.11) * (1.0 - transmit) * u_params[1].w * 0.25;

    // reflection
    vec3 V = reflect(rd, N);
    float tr;
    vec3 reflected = HitSphere(P, V, tr) ? BallShade(P + V * tr) : Sky(V);
    float cosT = clamp(dot(-rd, N), 0.0, 1.0);
    float fresnel = 0.02 + 0.98 * pow(1.0 - cosT, 5.0);
    vec3 water = mix(refracted, reflected, fresnel);

    vec3 result;
    if (base < 1.5) {          // water depth (surface to the refracted hit), blueprint ramp
        float d = clamp(tHit / 1.6, 0.0, 1.0);
        result = mix(vec3(0.85, 0.95, 1.0), vec3(0.02, 0.10, 0.35), d);
    }
    else if (base < 2.5) result = N * 0.5 + 0.5;   // surface normals
    else result = water;
    // wipe line
    if (stage - floor(stage) > 0.0 && abs(wipe - (stage - floor(stage))) * u_viewRect.z < 1.5) result = vec3(1.0, 0.75, 0.04) * 3.0;
    o_color = vec4(result, 1.0);
}
