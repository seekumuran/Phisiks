#version 460 core
// CAD ink outlines (Phisiks showcase post pass, see Render::PostPass): dark edge lines where the
// entity id changes (part silhouettes, also seen through ghosted casings, which write no ids) and
// where the linear depth jumps or folds (creases inside one part).
//   u_params[0]  x strength 0..1, y line width (px), z relative depth-jump threshold
//   u_params[1]  rgb ink colour (linear)
#include "common.glsl"

layout(binding = 0) uniform sampler2D u_sceneColor;
layout(binding = 1) uniform sampler2D u_sceneDepth;
layout(binding = 2) uniform usampler2D u_sceneIds;
uniform vec4 u_viewRect;
uniform vec2 u_targetSize;
uniform vec4 u_params[16];

in vec2 v_uv;
layout(location = 0) out vec4 o_color;

float LinearDepth(float d) {
    float ndc = d * 2.0 - 1.0;
    if (u_fogParams2.w > 0.5) return -(ndc - u_projection[3][2]) / u_projection[2][2];   // orthographic
    return u_projection[3][2] / (ndc + u_projection[2][2]);
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 lo = ivec2(u_viewRect.xy), hi = ivec2(u_viewRect.xy + u_viewRect.zw) - 1;
    vec4 color = texelFetch(u_sceneColor, p, 0);
    int w = int(max(u_params[0].y, 1.0) + 0.5);
    uint id = texelFetch(u_sceneIds, p, 0).r;
    float dc = texelFetch(u_sceneDepth, p, 0).r;
    float zc = dc < 1.0 ? LinearDepth(dc) : 1e6;
    float edge = 0.0;
    float zs[4];
    const ivec2 offsets[4] = ivec2[4](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));
    for (int k = 0; k < 4; ++k) {
        ivec2 q = clamp(p + offsets[k] * w, lo, hi);
        uint idn = texelFetch(u_sceneIds, q, 0).r;
        float dn = texelFetch(u_sceneDepth, q, 0).r;
        float zn = dn < 1.0 ? LinearDepth(dn) : 1e6;
        zs[k] = zn;
        if (idn != id) edge = 1.0;
        else if (id != 0u && abs(zn - zc) > u_params[0].z * zc) edge = max(edge, 0.85);
    }
    // creases: second difference of the linear depth (flat faces are close to linear)
    if (id != 0u && edge < 0.5) {
        float fold = max(abs(zs[0] + zs[1] - 2.0 * zc), abs(zs[2] + zs[3] - 2.0 * zc)) / max(zc, 1e-3);
        edge = max(edge, smoothstep(0.004, 0.012, fold) * 0.6);
    }
    vec3 ink = u_params[1].rgb;
    o_color = vec4(mix(color.rgb, ink, edge * clamp(u_params[0].x, 0.0, 1.0)), color.a);
}
