#version 460 core
// Robust contrast-adaptive sharpening (the RCAS pass of the FSR 1.0 algorithm; clean-room, mirrored
// on the CPU by RenderMath::Fsr::Rcas). A 5-tap cross; the negative lobe is limited per channel so
// the result never leaves the [min, max] of the neighbourhood or [0, 1], and is damped where the
// centre looks like isolated noise.
layout(binding = 0) uniform sampler2D u_input;   // EASU output (output resolution)
uniform float u_sharpness;                       // exp2(-stops): 1 = strongest
layout(location = 0) out vec4 o_color;

vec3 Tap(ivec2 p) { return texelFetch(u_input, clamp(p, ivec2(0), textureSize(u_input, 0) - 1), 0).rgb; }
float Luma(vec3 c) { return c.g + 0.5 * (c.r + c.b); }

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 cN = Tap(p + ivec2(0, 1)), cW = Tap(p + ivec2(-1, 0)), cC = Tap(p), cE = Tap(p + ivec2(1, 0)), cS = Tap(p + ivec2(0, -1));
    vec3 mn4 = min(min(cN, cS), min(cW, cE));
    vec3 mx4 = max(max(cN, cS), max(cW, cE));
    vec3 hitMin = mn4 / max(4.0 * mx4, vec3(1.0 / 65536.0));
    vec3 hitMax = (vec3(1.0) - mx4) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * u_sharpness;
    // noise: the centre luma far from the ring average relative to the local range
    float lN = Luma(cN), lW = Luma(cW), lC = Luma(cC), lE = Luma(cE), lS = Luma(cS);
    float range = max(max(max(lN, lW), max(lE, lS)), lC) - min(min(min(lN, lW), min(lE, lS)), lC);
    float nz = clamp(abs(0.25 * (lN + lW + lE + lS) - lC) / max(range, 1.0 / 65536.0), 0.0, 1.0);
    lobe *= 1.0 - 0.5 * nz;
    vec3 result = (lobe * (cN + cW + cE + cS) + cC) / (4.0 * lobe + 1.0);
    o_color = vec4(clamp(result, 0.0, 1.0), 1.0);
}
