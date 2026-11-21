// Relativistic Doppler colouring for particles (ParticleColor::Doppler). Hot-reloadable: run with
// --hot-reload and edit this file while the black-hole showcase is running.
//
//   beta  = |v| / c               (c = u_dopplerSpeed, the "speed of light" of the visualisation)
//   D     = 1 / (gamma (1 - beta cos theta))   theta: angle between v and the direction to the camera
//   D > 1 approaching -> blueshift, D < 1 receding -> redshift; observed intensity ~ D^3 (beaming)
vec3 DopplerShift(vec3 baseColor, vec3 velocity, vec3 toCamera, float c) {
    float speed = length(velocity);
    if (speed < 1e-5) return baseColor;
    float beta = clamp(speed / max(c, 1e-3), 0.0, 0.95);
    float gamma = inversesqrt(1.0 - beta * beta);
    float cosTheta = dot(velocity / speed, toCamera);
    float D = 1.0 / (gamma * (1.0 - beta * cosTheta));
    float shift = clamp(log2(D) * 1.8, -1.0, 1.0);
    vec3 blue = vec3(0.30, 0.55, 1.70);
    vec3 red = vec3(1.70, 0.38, 0.12);
    vec3 tint = shift > 0.0 ? mix(vec3(1.0), blue, shift) : mix(vec3(1.0), red, -shift);
    return baseColor * tint * min(D * D * D, 8.0);
}
