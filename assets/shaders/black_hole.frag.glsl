#version 460 core
// Showcase 03: Schwarzschild black hole with gravitational lensing (Render::PostPass).
//
// Every view ray is a null geodesic. In its orbital plane (spanned by the camera direction e1
// from the hole and the in-plane ray direction e2) the photon orbit obeys the Binet equation
//     u'' + u = 3 M u^2        (u = 1 / r, ' = d/dphi, units of M)
// integrated with RK4 in phi. The ray is captured when r <= 2M (event horizon -> shadow), escapes
// when u crosses 0 (the asymptotic direction picks the background star), and every crossing of
// the accretion-disk plane between r_in and r_out samples the disk: the density of the integrated
// disk particles (u_data0, polar bins) shaded by a thin-disk temperature profile, gravitational +
// relativistic Doppler factor g (beaming g^n), front-to-back composited. Photons grazing the
// photon sphere (r = 3M) add the thin Einstein photon ring. Includes doppler.glsl (hot-reloadable).
//   u_params[0] centre xyz (world), M (metres)
//   u_params[1] disk normal xyz, Doppler scale (0 = no Doppler)
//   u_params[2] r_in, r_out (M), beaming exponent, disk brightness
//   u_params[3] split-test tint (0/1), raw particles view (0/1), star brightness, photon-ring gain
//   u_params[4] disk azimuth-0 axis xyz (in the disk plane)
#include "common.glsl"
#include "doppler.glsl"

layout(binding = 0) uniform sampler2D u_sceneColor;
layout(binding = 1) uniform sampler2D u_sceneDepth;
layout(binding = 3) uniform sampler2D u_data0;
uniform vec4 u_viewRect;
uniform vec2 u_targetSize;
uniform vec4 u_params[16];

in vec2 v_uv;
layout(location = 0) out vec4 o_color;

float Hash(vec3 p) {
    p = fract(p * vec3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

// Procedural starfield + faint galactic band on the celestial sphere.
vec3 Stars(vec3 d) {
    vec3 a = abs(d);
    float m = max(a.x, max(a.y, a.z));
    vec3 face = a.x == m ? vec3(sign(d.x), 0, 0) : a.y == m ? vec3(0, sign(d.y), 0) : vec3(0, 0, sign(d.z));
    vec2 uv = (a.x == m ? d.yz : a.y == m ? d.xz : d.xy) / m;
    vec3 col = vec3(0.0);
    for (int layer = 0; layer < 2; ++layer) {
        float res = layer == 0 ? 70.0 : 220.0;
        vec2 g = uv * res;
        vec2 cell = floor(g);
        float h = Hash(vec3(cell, dot(face, vec3(1, 2, 3)) + float(layer) * 7.0));
        if (h < (layer == 0 ? 0.035 : 0.07)) {
            vec2 c = cell + 0.5 + (vec2(Hash(vec3(cell, 3.1)), Hash(vec3(cell, 5.7))) - 0.5) * 0.4;
            float dist = length(g - c);
            float b = pow(Hash(vec3(cell, 9.3)), 6.0) * (layer == 0 ? 6.0 : 1.5) + 0.15;
            float t = Hash(vec3(cell, 1.7));
            vec3 tint = mix(vec3(1.0, 0.72, 0.5), vec3(0.65, 0.78, 1.0), t);
            col += tint * b * exp(-dist * dist * (layer == 0 ? 40.0 : 60.0));
        }
    }
    // galactic band
    vec3 bandN = normalize(vec3(0.25, 0.9, 0.35));
    float band = exp(-pow(dot(d, bandN), 2.0) * 28.0);
    float n = Hash(floor(d * 40.0)) * 0.5 + Hash(floor(d * 13.0)) * 0.5;
    col += vec3(0.55, 0.5, 0.62) * band * (0.04 + 0.05 * n);
    return col;
}

// Thin-disk temperature profile (peak 1 near r = 8M), Novikov-Thorne-like shape.
float Temperature(float r) {
    float rin = u_params[2].x;
    return pow(r / 8.0, -0.75) * pow(max(1.0 - sqrt(rin / r), 0.0), 0.25) * 1.75;
}
vec3 Blackbody(float t) {
    vec3 cool = vec3(1.0, 0.30, 0.05), warm = vec3(1.0, 0.72, 0.42), hot = vec3(0.70, 0.82, 1.0);
    return t < 1.0 ? mix(cool, warm, smoothstep(0.35, 1.0, t)) : mix(warm, hot, smoothstep(1.0, 1.7, t));
}

vec3 DiskBasis1(vec3 n) { return normalize(u_params[4].xyz - n * dot(u_params[4].xyz, n)); }   // disk azimuth 0 (u_params[4])

// Disk emission + opacity at a plane crossing (position p in M, traced direction k).
vec4 DiskSample(vec3 p, vec3 k, vec3 n) {
    float r = length(p);
    float rin = u_params[2].x, rout = u_params[2].y;
    if (r < rin || r > rout) return vec4(0.0);
    vec3 b1 = DiskBasis1(n), b2 = cross(n, b1);
    float phi = atan(dot(p, b2), dot(p, b1));
    vec4 dens = texture(u_data0, vec2(phi / (2.0 * PI) + 0.5, (r - rin) / (rout - rin)));
    float density = dens.a;
    // orbital velocity (Paczynski-Wiita circular orbit) and the Doppler factor
    float v = clamp(sqrt(r) / (r - 2.0), 0.0, 0.95) * u_params[1].w;
    vec3 vdir = normalize(cross(n, p));
    float g = DopplerFactor(v, dot(vdir, -k)) * sqrt(max(1.0 - 2.0 / r, 0.05)) / sqrt(1.0 - 2.0 / 8.0);   // Doppler x gravitational redshift (1 at rest at r = 8M)
    float temp = Temperature(r) * g;
    vec3 col = Blackbody(temp);
    if (u_params[3].x > 0.5 && dens.a > 1e-4) col = (dens.r * vec3(1.0, 0.35, 0.12) + dens.g * vec3(0.3, 1.0, 0.35) + dens.b * vec3(0.3, 0.55, 1.0)) / dens.a;
    float beaming = pow(max(g, 0.0), u_params[2].z);
    float edge = smoothstep(rin, rin + 0.6, r) * (1.0 - smoothstep(rout - 4.0, rout, r));
    float alpha = clamp(density * 0.55, 0.0, 0.92) * edge;
    vec3 emit = col * temp * temp * beaming * u_params[2].w * (0.35 + density);
    return vec4(emit, alpha);
}

void main() {
    ivec2 pix = ivec2(gl_FragCoord.xy);
    vec3 scene = texelFetch(u_sceneColor, pix, 0).rgb;
    float depth = texelFetch(u_sceneDepth, pix, 0).r;
    vec2 ndcXY = (gl_FragCoord.xy - u_viewRect.xy) / u_viewRect.zw * 2.0 - 1.0;
    vec4 nearH = u_inverseViewProjection * vec4(ndcXY, -1.0, 1.0);
    vec4 farH = u_inverseViewProjection * vec4(ndcXY, 1.0, 1.0);
    vec3 roW = nearH.xyz / nearH.w;
    vec3 rd = normalize(farH.xyz / farH.w - roW);
    float M = max(u_params[0].w, 1e-3);
    vec3 o = (roW - u_params[0].xyz) / M;
    vec3 n = normalize(u_params[1].xyz);

    // raw particles view: straight rays, disk density only
    if (u_params[3].y > 0.5) {
        float t = -dot(o, n) / dot(rd, n);
        vec3 col = vec3(0.0);
        if (t > 0.0) {
            vec4 d = DiskSample(o + rd * t, rd, n);
            col = vec3(1.0, 0.55, 0.2) * d.a * 2.0;
        }
        o_color = vec4(scene + col, 1.0);
        return;
    }
    if (depth < 1.0) { o_color = vec4(scene, 1.0); return; }   // geometry in front (probes, horizon mesh)

    float r0 = length(o);
    vec3 e1 = o / r0;
    float cosA = dot(rd, e1);
    vec3 side = rd - cosA * e1;
    float sinA = length(side);
    vec3 e2 = sinA > 1e-6 ? side / sinA : normalize(abs(e1.y) < 0.9 ? cross(e1, vec3(0, 1, 0)) : cross(e1, vec3(1, 0, 0)));
    float u = 1.0 / r0;
    float du = sinA > 1e-6 ? -u * cosA / sinA : -1e3;
    float a1 = dot(e1, n), a2 = dot(e2, n);

    vec3 col = vec3(0.0);
    float alpha = 0.0;
    float ring = 0.0;
    float phi = 0.0;
    float f = a1;   // disk-plane function cos(phi) a1 + sin(phi) a2
    bool captured = false, escaped = false;
    vec3 escapeDir = rd;
    for (int i = 0; i < 160; ++i) {
        float h = mix(0.16, 0.025, clamp(u * 3.5, 0.0, 1.0));
        // RK4 on (u, u'):  u'' = 3 u^2 - u
        float k1u = du,                 k1v = 3.0 * u * u - u;
        float u2 = u + 0.5 * h * k1u,   v2 = du + 0.5 * h * k1v;
        float k2u = v2,                 k2v = 3.0 * u2 * u2 - u2;
        float u3 = u + 0.5 * h * k2u,   v3 = du + 0.5 * h * k2v;
        float k3u = v3,                 k3v = 3.0 * u3 * u3 - u3;
        float u4 = u + h * k3u,         v4 = du + h * k3v;
        float k4u = v4,                 k4v = 3.0 * u4 * u4 - u4;
        float uNext = u + h / 6.0 * (k1u + 2.0 * k2u + 2.0 * k3u + k4u);
        float duNext = du + h / 6.0 * (k1v + 2.0 * k2v + 2.0 * k3v + k4v);
        float phiNext = phi + h;
        if (uNext <= 0.0) {   // escaped: asymptotic direction at the zero crossing of u
            float s = u / max(u - uNext, 1e-9);
            float pe = phi + h * s;
            escapeDir = cos(pe) * e1 + sin(pe) * e2;
            escaped = true;
            break;
        }
        float fNext = cos(phiNext) * a1 + sin(phiNext) * a2;
        if (f * fNext < 0.0 && alpha < 0.98) {   // disk plane crossing
            float s = f / (f - fNext);
            float pc = phi + h * s;
            float uc = mix(u, uNext, s), duc = mix(du, duNext, s);
            float rc = 1.0 / max(uc, 1e-6);
            vec3 radial = cos(pc) * e1 + sin(pc) * e2, tangent = -sin(pc) * e1 + cos(pc) * e2;
            vec3 k = normalize(radial * (-duc / (uc * uc)) + tangent * rc);
            vec4 d = DiskSample(radial * rc, k, n);
            col += (1.0 - alpha) * d.rgb * d.a;
            alpha += (1.0 - alpha) * d.a;
        }
        ring += exp(-pow((uNext - 1.0 / 3.0) * 90.0, 2.0)) * h;
        u = uNext; du = duNext; phi = phiNext; f = fNext;
        if (u >= 0.5) { captured = true; break; }
    }
    vec3 background = vec3(0.0);
    if (escaped) background = Stars(escapeDir) * u_params[3].z;
    else if (!captured) background = Stars(cos(phi) * e1 + sin(phi) * e2) * u_params[3].z * 0.3;
    vec3 photonRing = vec3(1.0, 0.82, 0.6) * ring * u_params[3].w;
    vec3 result = col + (1.0 - alpha) * (background + (captured ? vec3(0.0) : photonRing));
    o_color = vec4(result + scene, 1.0);
}
