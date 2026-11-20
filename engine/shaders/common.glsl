// Shared declarations. Layouts must match the C++ structs in engine/src/render/Renderer.cpp.
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
layout(std430, binding = 1) readonly buffer LightBuffer { Light u_lights[]; };

struct Instance {
    mat4 model;
    mat4 normalMatrix;
    vec4 scaleMaterial;         // xyz object scale (pattern metres), w material index
    uvec4 ids;                  // x entity id + 1 (0 = none)
    vec4 uvScale;               // xy
};
layout(std430, binding = 2) readonly buffer InstanceBuffer { Instance u_instances[]; };

struct Material {
    vec4 baseRoughness;         // rgb base colour (linear), a roughness
    vec4 patternMetallic;       // rgb pattern colour, a metallic
    vec4 emissivePattern;       // rgb emissive, a pattern id
    vec4 params;                // pattern scale, line width, pattern roughness (<0: same), pattern glow
    vec4 textureParams;         // x albedo texture layer (-1 none), y repeats per metre
};
layout(std430, binding = 3) readonly buffer MaterialBuffer { Material u_materials[]; };
