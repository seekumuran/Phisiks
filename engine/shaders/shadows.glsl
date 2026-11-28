// Shadow lookups: directional (2D), spot (2D array) and point lights (cube array storing
// distance / range). Hardware depth comparison with bilinear filtering, plus 3x3 PCF for 2D maps.
layout(binding = 4) uniform sampler2DShadow u_dirShadow;
layout(binding = 5) uniform sampler2DArrayShadow u_spotShadows;
layout(binding = 6) uniform samplerCubeArrayShadow u_pointShadows;

float DirectionalShadow(vec3 worldPos, vec3 N, vec3 L) {
    float texelWorld = u_fogParams2.z;
    vec3 p = worldPos + N * texelWorld * (1.5 + 2.0 * (1.0 - clamp(dot(N, L), 0.0, 1.0)));
    vec4 clip = u_dirShadowMatrix * vec4(p, 1.0);
    vec3 uvz = clip.xyz / clip.w * 0.5 + 0.5;
    if (any(lessThan(uvz, vec3(0.0))) || any(greaterThan(uvz, vec3(1.0)))) return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(u_dirShadow, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sum += texture(u_dirShadow, vec3(uvz.xy + vec2(x, y) * texel, uvz.z - 0.0005));
    return sum / 9.0;
}

float SpotShadow(int index, vec3 worldPos, vec3 N) {
    vec4 clip = u_spotShadowMatrices[index] * vec4(worldPos + N * 0.03, 1.0);
    vec3 uvz = clip.xyz / clip.w * 0.5 + 0.5;
    if (any(lessThan(uvz, vec3(0.0))) || any(greaterThan(uvz, vec3(1.0)))) return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(u_spotShadows, 0).xy);
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sum += texture(u_spotShadows, vec4(uvz.xy + vec2(x, y) * texel, float(index), uvz.z - 0.0008));
    return sum / 9.0;
}

float PointShadow(int index, vec3 worldPos, vec3 N, vec3 lightPos, float range) {
    vec3 d = worldPos + N * 0.04 - lightPos;
    float reference = length(d) / range - 0.004;
    return texture(u_pointShadows, vec4(d, float(index)), reference);
}
