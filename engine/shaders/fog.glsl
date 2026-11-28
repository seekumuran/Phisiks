// Analytic exponential distance fog + exponential height fog: closed-form optical depth along
// the view ray. Mirrors RenderMath::FogOpticalDepth / FogAmount.
float FogAmount(vec3 origin, vec3 dir, float distance) {
    if (u_fogColor.w < 0.5 || distance <= 0.0) return 0.0;
    float distanceDepth = u_fogParams.x * distance;
    float k = u_fogParams.z * dir.y * distance;
    float shape = abs(k) > 1e-4 ? (1.0 - exp(-k)) / k : 1.0 - 0.5 * k;
    float heightDepth = u_fogParams.y * exp(-u_fogParams.z * (origin.y - u_fogParams.w)) * distance * shape;
    return min(1.0 - exp(-(distanceDepth + heightDepth)), u_fogParams2.x);
}
