#version 460 core
// Edge-adaptive spatial upsampling (the EASU pass of the FSR 1.0 algorithm; clean-room, written
// from AMD's public description of the method, mirrored on the CPU by RenderMath::Fsr::Easu).
//   1. 12 texels around the output position's 2x2 input quad.
//   2. Luma gradients of the four quad texels (bilinearly weighted) give the local edge direction
//      and an edge strength: strong edges -> anisotropic kernel stretched along the edge.
//   3. Lanczos-2 approximation (polynomial window, negative-lobe strength follows the edge
//      strength), weights normalised; result clamped to the quad's min/max (no ringing).
layout(binding = 0) uniform sampler2D u_input;   // LDR (display-encoded) at the internal resolution
uniform vec2 u_inputSize;
uniform vec2 u_outputSize;
layout(location = 0) out vec4 o_color;

vec3 Tap(ivec2 base, ivec2 offset) {
    ivec2 p = clamp(base + offset, ivec2(0), ivec2(u_inputSize) - 1);
    return texelFetch(u_input, p, 0).rgb;
}
float Luma(vec3 c) { return c.g + 0.5 * (c.r + c.b); }

void Accumulate(vec2 offset, vec2 dir, vec2 len2, float lob, float clp, vec3 c, inout vec3 acc, inout float wsum) {
    vec2 v = vec2(dot(offset, dir), dot(offset, vec2(-dir.y, dir.x))) * len2;
    float d2 = min(dot(v, v), clp);
    float wB = 0.4 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 1.5625 * wB - 0.5625;
    float w = wB * wA;
    acc += c * w;
    wsum += w;
}

// Direction / edge-strength contribution of one quad texel (luma of its 4 neighbours).
void Analyse(inout vec2 dir, inout float len, float w, float lL, float lR, float lD, float lU, float lC) {
    float dx = lR - lL;
    float dy = lU - lD;
    float ex = clamp(abs(dx) / max(max(abs(lR - lC), abs(lC - lL)), 1.0 / 65536.0), 0.0, 1.0);
    float ey = clamp(abs(dy) / max(max(abs(lU - lC), abs(lC - lD)), 1.0 / 65536.0), 0.0, 1.0);
    dir += vec2(dx, dy) * w;
    len += (ex * ex + ey * ey) * w;
}

void main() {
    vec2 pp = gl_FragCoord.xy * (u_inputSize / u_outputSize) - 0.5;
    vec2 fp = floor(pp);
    vec2 f = pp - fp;
    ivec2 base = ivec2(fp);
    //      b c          y + 2: n o
    //    e f g h        y + 1: i j k l
    //    i j k l        y    : e f g h
    //      n o          y - 1:   b c
    vec3 b = Tap(base, ivec2(0, -1)), c = Tap(base, ivec2(1, -1));
    vec3 e = Tap(base, ivec2(-1, 0)), ff = Tap(base, ivec2(0, 0)), g = Tap(base, ivec2(1, 0)), h = Tap(base, ivec2(2, 0));
    vec3 i = Tap(base, ivec2(-1, 1)), j = Tap(base, ivec2(0, 1)), k = Tap(base, ivec2(1, 1)), l = Tap(base, ivec2(2, 1));
    vec3 n = Tap(base, ivec2(0, 2)), o = Tap(base, ivec2(1, 2));
    float lb = Luma(b), lc = Luma(c), le = Luma(e), lf = Luma(ff), lg = Luma(g), lh = Luma(h);
    float li = Luma(i), lj = Luma(j), lk = Luma(k), ll = Luma(l), ln = Luma(n), lo = Luma(o);

    vec2 dir = vec2(0.0);
    float len = 0.0;
    Analyse(dir, len, (1.0 - f.x) * (1.0 - f.y), le, lg, lb, lj, lf);
    Analyse(dir, len, f.x * (1.0 - f.y), lf, lh, lc, lk, lg);
    Analyse(dir, len, (1.0 - f.x) * f.y, li, lk, lf, ln, lj);
    Analyse(dir, len, f.x * f.y, lj, ll, lg, lo, lk);

    float dir2 = dot(dir, dir);
    bool flat_ = dir2 < 1.0 / 32768.0;
    dir = flat_ ? vec2(1.0, 0.0) : dir * inversesqrt(dir2);
    len = 0.5 * len;
    len *= len;
    float stretch = 1.0 / max(abs(dir.x), abs(dir.y));   // 1 axis-aligned .. sqrt(2) diagonal
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + (0.25 - 0.04 - 0.5) * len;
    float clp = 1.0 / lob;

    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    Accumulate(vec2(0.0, -1.0) - f, dir, len2, lob, clp, b, acc, wsum);
    Accumulate(vec2(1.0, -1.0) - f, dir, len2, lob, clp, c, acc, wsum);
    Accumulate(vec2(-1.0, 0.0) - f, dir, len2, lob, clp, e, acc, wsum);
    Accumulate(vec2(0.0, 0.0) - f, dir, len2, lob, clp, ff, acc, wsum);
    Accumulate(vec2(1.0, 0.0) - f, dir, len2, lob, clp, g, acc, wsum);
    Accumulate(vec2(2.0, 0.0) - f, dir, len2, lob, clp, h, acc, wsum);
    Accumulate(vec2(-1.0, 1.0) - f, dir, len2, lob, clp, i, acc, wsum);
    Accumulate(vec2(0.0, 1.0) - f, dir, len2, lob, clp, j, acc, wsum);
    Accumulate(vec2(1.0, 1.0) - f, dir, len2, lob, clp, k, acc, wsum);
    Accumulate(vec2(2.0, 1.0) - f, dir, len2, lob, clp, l, acc, wsum);
    Accumulate(vec2(0.0, 2.0) - f, dir, len2, lob, clp, n, acc, wsum);
    Accumulate(vec2(1.0, 2.0) - f, dir, len2, lob, clp, o, acc, wsum);

    vec3 lo4 = min(min(ff, g), min(j, k));
    vec3 hi4 = max(max(ff, g), max(j, k));
    vec3 result = clamp(acc / wsum, lo4, hi4);
    o_color = vec4(result, 1.0);
}
