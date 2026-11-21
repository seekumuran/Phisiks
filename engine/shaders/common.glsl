// Shared declarations. Layouts must match the C++ structs in engine/src/render/Renderer.cpp.
// Storage is read through Load*() accessors: shader-storage buffers on the OpenGL 4.6 tier, texture
// buffers (texelFetch, RGBA32F texels) on the 4.1 tier (PHISIKS_GL41: macOS, older iGPUs). The
// renderer rewrites `layout(binding = N)` for 4.1 and sets the units / block bindings after linking.
const float PI = 3.14159265358979;

layout(std140, binding = 0) uniform FrameData {
    mat4 u_view;
    mat4 u_projection;
    mat4 u_viewProjection;
    mat4 u_inverseViewProjection;
    vec4 u_cameraPosition;      // xyz, w = time (s)
    vec4 u_skyZenith;           // rgb, w = ambient intensity
    vec4 u_skyHorizon;          // rgb, w = exposure
    vec4 u_groundColor;         // rgb, w = light count
    vec4 u_fogColor;            // rgb, w = enabled
    vec4 u_fogParams;           // distance density, height density, height falloff, height base
    vec4 u_fogParams2;          // max opacity, shadows enabled, directional texel (m), orthographic view (1/0)
    vec4 u_ambientUp;           // rgb ambient from above (x ambient intensity)
    vec4 u_ambientDown;         // rgb ambient from below
    vec4 u_rim;                 // rgb rim light colour x intensity, w = rim exponent
    mat4 u_dirShadowMatrix;
    mat4 u_spotShadowMatrices[4];
};

struct Light {
    vec4 positionRange;         // xyz position, w range
    vec4 directionType;         // xyz direction the light travels, w type (0 dir, 1 point, 2 spot)
    vec4 colorIntensity;        // rgb colour, w intensity
    vec4 params;                // cos inner, cos outer, shadow index (-1 none), unused
};

struct Instance {
    mat4 model;
    mat4 normalMatrix;
    vec4 scaleMaterial;         // xyz object scale (pattern metres), w material index
    uvec4 ids;                  // x entity id + 1 (0 = none)
    vec4 uvScale;               // xy
};

struct Material {
    vec4 baseRoughness;         // rgb base colour (linear), a roughness
    vec4 patternMetallic;       // rgb pattern colour, a metallic
    vec4 emissivePattern;       // rgb emissive, a pattern id
    vec4 params;                // pattern scale, line width, pattern roughness (<0: same), pattern glow
    vec4 textureParams;         // x albedo texture layer (-1 none), y repeats per metre
};

#ifdef PHISIKS_GL41
layout(binding = 9) uniform samplerBuffer u_lightTexels;       // 4 texels per light
layout(binding = 10) uniform samplerBuffer u_instanceTexels;   // 11 texels per instance (ids stored as float values)
layout(binding = 11) uniform samplerBuffer u_materialTexels;   // 5 texels per material
uniform int u_instanceBase;                                    // texel of instance 0 (per-frame ring region)
Light LoadLight(uint i) {
    int b = int(i) * 4;
    Light l;
    l.positionRange = texelFetch(u_lightTexels, b);
    l.directionType = texelFetch(u_lightTexels, b + 1);
    l.colorIntensity = texelFetch(u_lightTexels, b + 2);
    l.params = texelFetch(u_lightTexels, b + 3);
    return l;
}
Instance LoadInstance(uint i) {
    int b = u_instanceBase + int(i) * 11;
    Instance s;
    s.model = mat4(texelFetch(u_instanceTexels, b), texelFetch(u_instanceTexels, b + 1), texelFetch(u_instanceTexels, b + 2), texelFetch(u_instanceTexels, b + 3));
    s.normalMatrix = mat4(texelFetch(u_instanceTexels, b + 4), texelFetch(u_instanceTexels, b + 5), texelFetch(u_instanceTexels, b + 6), texelFetch(u_instanceTexels, b + 7));
    s.scaleMaterial = texelFetch(u_instanceTexels, b + 8);
    s.ids = uvec4(texelFetch(u_instanceTexels, b + 9) + 0.5);
    s.uvScale = texelFetch(u_instanceTexels, b + 10);
    return s;
}
Material LoadMaterial(uint i) {
    int b = int(i) * 5;
    Material m;
    m.baseRoughness = texelFetch(u_materialTexels, b);
    m.patternMetallic = texelFetch(u_materialTexels, b + 1);
    m.emissivePattern = texelFetch(u_materialTexels, b + 2);
    m.params = texelFetch(u_materialTexels, b + 3);
    m.textureParams = texelFetch(u_materialTexels, b + 4);
    return m;
}
#else
layout(std430, binding = 1) readonly buffer LightBuffer { Light u_lights[]; };
layout(std430, binding = 2) readonly buffer InstanceBuffer { Instance u_instances[]; };
layout(std430, binding = 3) readonly buffer MaterialBuffer { Material u_materials[]; };
Light LoadLight(uint i) { return u_lights[i]; }
Instance LoadInstance(uint i) { return u_instances[i]; }
Material LoadMaterial(uint i) { return u_materials[i]; }
#endif
