// Procedural surface patterns, mapped tri-planar in object space scaled to metres (so they do
// not stretch on scaled boxes and do not swim on moving objects).
layout(binding = 7) uniform sampler2D u_font;          // built-in 5x7 font atlas (calibration grid labels)
layout(binding = 8) uniform sampler2DArray u_albedoTextures;   // material albedo textures (sRGB), one per layer

float Hash12(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

float ValueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = Hash12(i), b = Hash12(i + vec2(1, 0)), c = Hash12(i + vec2(0, 1)), d = Hash12(i + vec2(1, 1));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float Fbm(vec2 p) {
    float value = 0.0, amplitude = 0.5;
    for (int i = 0; i < 4; ++i) {
        value += amplitude * ValueNoise(p);
        p = p * 2.03 + 17.1;
        amplitude *= 0.5;
    }
    return value;
}

// 1 on anti-aliased lines of `width` (pattern units) at integer coordinates.
float LineMask(vec2 p, float width) {
    vec2 aa = max(fwidth(p), vec2(1e-5));
    vec2 g = abs(fract(p - 0.5) - 0.5);
    vec2 line = 1.0 - smoothstep(vec2(width * 0.5) - aa, vec2(width * 0.5) + aa, g);
    return max(line.x, line.y);
}

vec3 Rainbow(float hue) {
    return clamp(abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
}

// Font atlas lookup: uv in [0,1)^2 of the glyph box, y up.
float GlyphMask(int code, vec2 uv) {
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) return 0.0;
    int cell = code - 32;
    ivec2 texel = ivec2((cell % 16) * 6 + int(uv.x * 5.0), (cell / 16) * 8 + int((1.0 - uv.y) * 7.0));
    return texelFetch(u_font, texel, 0).r;
}

// Returns rgb = albedo, a = pattern amount (drives roughness); `glow` = self-illumination weight.
vec4 PatternPlanar(Material m, uint pattern, vec2 p, out float glow) {
    glow = 0.0;
    vec3 base = m.baseRoughness.rgb;
    vec3 accent = m.patternMetallic.rgb;
    float lineWidth = m.params.y;
    if (pattern == 1u) {                                   // checker
        vec2 c = floor(p);
        float t = mod(c.x + c.y, 2.0);
        return vec4(mix(base, accent, t), t);
    }
    if (pattern == 2u) {                                   // grid: major + minor lines
        float t = max(LineMask(p, lineWidth), LineMask(p * 10.0, lineWidth * 4.0) * 0.35);
        return vec4(mix(base, accent, t), t);
    }
    if (pattern == 3u) {                                   // noise
        float t = smoothstep(0.25, 0.75, Fbm(p));
        return vec4(mix(base, accent, t), t);
    }
    if (pattern == 4u) {                                   // wood: long grain along V
        float rings = fract((p.x + Fbm(p * vec2(0.35, 0.12)) * 0.6) * 3.0);
        float streaks = ValueNoise(p * vec2(30.0, 1.5));
        float t = smoothstep(0.3, 0.7, rings) * 0.7 + streaks * 0.3;
        return vec4(mix(base, accent, t), t);
    }
    if (pattern == 5u) {                                   // tiles with grout
        float t = LineMask(p, lineWidth);
        return vec4(mix(base, accent, t) * (0.92 + 0.16 * Hash12(floor(p))), t);
    }
    if (pattern == 6u) {                                   // calibration grid: rainbow columns, labelled tiles
        vec2 cell = floor(p);
        vec2 f = p - cell;
        vec3 tint = Rainbow(mod(cell.x, 10.0) / 10.0);
        bool odd = mod(cell.x + cell.y, 2.0) > 0.5;
        vec3 color = odd ? base + tint * 0.12 : base;
        // Coloured inner frame
        vec2 edge = min(f, 1.0 - f);
        float frame = (1.0 - smoothstep(0.035, 0.045, min(edge.x, edge.y))) * step(0.02, min(edge.x, edge.y));
        color = mix(color, tint * 0.55, frame);
        // Label: column letter A-J + row digit 0-9 in the tile's lower-left corner
        int letter = 65 + int(mod(cell.x, 10.0));
        int digit = 48 + int(mod(cell.y, 10.0));
        vec2 box = (f - vec2(0.1, 0.1)) / vec2(0.18, 0.252);
        float label = max(GlyphMask(letter, box), GlyphMask(digit, box - vec2(1.2, 0.0)));
        color = mix(color, tint, label);
        // Light grid lines on the tile borders
        float line = LineMask(p, lineWidth);
        color = mix(color, accent, line);
        glow = max(label, line * 0.35);
        return vec4(color, max(label, line));
    }
    return vec4(base, 0.0);
}

vec4 SamplePattern(Material m, vec3 objectPosMetres, vec3 objectNormal, vec2 uvScale, out float glow) {
    glow = 0.0;
    uint pattern = uint(m.emissivePattern.w + 0.5);
    if (pattern == 0u) return vec4(m.baseRoughness.rgb, 0.0);
    vec3 p = objectPosMetres * m.params.x;
    vec3 w = pow(abs(objectNormal), vec3(8.0));
    w /= max(w.x + w.y + w.z, 1e-5);
    vec4 result = vec4(0.0);
    float g;
    if (w.x > 0.01) { result += w.x * PatternPlanar(m, pattern, p.zy * uvScale, g); glow += w.x * g; }
    if (w.y > 0.01) { result += w.y * PatternPlanar(m, pattern, vec2(p.x, -p.z) * uvScale, g); glow += w.y * g; }   // labels read "up" = -Z
    if (w.z > 0.01) { result += w.z * PatternPlanar(m, pattern, p.xy * uvScale, g); glow += w.z * g; }
    return result;
}

// Albedo texture mapped tri-planar like the patterns (1.0 when the material has no texture).
vec3 SampleAlbedoTexture(Material m, vec3 objectPosMetres, vec3 objectNormal, vec2 uvScale) {
    if (m.textureParams.x < 0.0) return vec3(1.0);
    float layer = m.textureParams.x;
    vec3 p = objectPosMetres * m.textureParams.y;
    vec3 w = pow(abs(objectNormal), vec3(8.0));
    w /= max(w.x + w.y + w.z, 1e-5);
    vec3 result = vec3(0.0);
    if (w.x > 0.01) result += w.x * texture(u_albedoTextures, vec3(p.zy * uvScale, layer)).rgb;
    if (w.y > 0.01) result += w.y * texture(u_albedoTextures, vec3(vec2(p.x, -p.z) * uvScale, layer)).rgb;
    if (w.z > 0.01) result += w.z * texture(u_albedoTextures, vec3(p.xy * uvScale, layer)).rgb;
    return result;
}
