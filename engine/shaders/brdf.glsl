// Cook-Torrance BRDF (GGX normal distribution, height-correlated Smith visibility, Schlick
// Fresnel) plus light falloff. Mirrors engine/src/render/RenderMath.cpp.
float D_GGX(float NoH, float alpha) {
    float a2 = alpha * alpha;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float V_SmithGGXCorrelated(float NoV, float NoL, float alpha) {
    float a2 = alpha * alpha;
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-6);
}

vec3 F_Schlick(float VoH, vec3 f0) {
    return f0 + (vec3(1.0) - f0) * pow(1.0 - clamp(VoH, 0.0, 1.0), 5.0);
}

float DistanceAttenuation(float distance, float range) {
    float d2 = max(distance * distance, 1e-4);
    float ratio = distance / max(range, 1e-4);
    float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
    return window * window / d2;
}

float SpotFactor(float cosAngle, float cosInner, float cosOuter) {
    float t = clamp((cosAngle - cosOuter) / max(cosInner - cosOuter, 1e-4), 0.0, 1.0);
    return t * t;
}

// Reflected radiance per unit light radiance for one light.
vec3 ShadeLight(vec3 N, vec3 V, vec3 L, vec3 albedo, float roughness, float metallic) {
    vec3 H = normalize(V + L);
    float NoV = max(dot(N, V), 1e-4);
    float NoL = clamp(dot(N, L), 0.0, 1.0);
    float NoH = clamp(dot(N, H), 0.0, 1.0);
    float VoH = clamp(dot(V, H), 0.0, 1.0);
    float alpha = max(roughness * roughness, 0.002);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = F_Schlick(VoH, f0);
    vec3 specular = D_GGX(NoH, alpha) * V_SmithGGXCorrelated(NoV, NoL, alpha) * F;
    vec3 diffuse = (1.0 - metallic) * (vec3(1.0) - F) * albedo / PI;
    return (diffuse + specular) * NoL;
}
