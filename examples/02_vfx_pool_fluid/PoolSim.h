#pragma once
// Pool water simulation of showcase 02 (no rendering; unit-tested).
//
//   * Heightfield: N x N water columns over the pool (cell size dx), the discrete wave equation in
//     the common "average of the neighbours" form (velocity += (neighbour mean - h) * k, damping),
//     reflecting walls; the mean height is conserved exactly by the update.
//   * Two-way sphere coupling: each column's length inside the sphere and below the free surface
//     is tracked; the change since the last step is removed from the column (displaced water
//     rises around a sinking ball, falls behind a rising one). Buoyancy uses the analytic
//     submerged volume of the sphere cap below the local surface, F = rho g V, plus drag.
//   * Caustics: sun rays are refracted at every (2x supersampled) surface point with Snell's law
//     (n = 1.333) and the light energy of each surface patch is deposited where its ray meets the
//     pool floor; intensity = deposited energy / bin area (1 = flat water). Rays blocked by the
//     sphere deposit nothing (the ball's caustic shadow).
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Showcase::Pool {
    constexpr float WATER_IOR = 1.333f;
    constexpr float PI = 3.14159265f;

    // Snell refraction of unit `d` at unit normal `n` (n against d), eta = n1 / n2. Zero on total internal reflection.
    inline glm::vec3 Refract(const glm::vec3& d, const glm::vec3& n, float eta) {
        const float c = -glm::dot(n, d);
        const float k = 1.0f - eta * eta * (1.0f - c * c);
        if (k < 0.0f) return glm::vec3(0.0f);
        return eta * d + (eta * c - std::sqrt(k)) * n;
    }

    // Volume of a sphere (radius r) below a plane at depth d into it (0 <= d <= 2r).
    inline float CapVolume(float r, float d) {
        d = std::clamp(d, 0.0f, 2.0f * r);
        return PI * d * d * (3.0f * r - d) / 3.0f;
    }

    struct Sphere { glm::vec3 centre{ 0.0f }; float radius = 0.35f; };

    class HeightField {
    public:
        int n = 128;
        float halfSize = 2.0f;        // pool half extent (m), square
        float level = 1.2f;           // rest water level (world y)
        float floorY = 0.25f;         // pool floor (world y)
        float waveK = 0.9f;           // neighbour coupling per step (stable well below 2)
        float damping = 0.993f;
        std::vector<float> h, v;      // height offset above `level`, vertical velocity per step
        std::vector<float> occupied;  // column length inside the sphere below the surface (last step)
        std::vector<float> caustics;  // n x n floor intensity (1 = flat water)

        void Reset(int size, float half, float waterLevel, float floorLevel) {
            n = size; halfSize = half; level = waterLevel; floorY = floorLevel;
            h.assign(size_t(n) * n, 0.0f);
            v.assign(size_t(n) * n, 0.0f);
            occupied.assign(size_t(n) * n, 0.0f);
            caustics.assign(size_t(n) * n, 1.0f);
        }
        float Cell() const { return 2.0f * halfSize / float(n); }
        glm::vec2 CellCentre(int i, int j) const { return glm::vec2(-halfSize + (float(i) + 0.5f) * Cell(), -halfSize + (float(j) + 0.5f) * Cell()); }
        float& H(int i, int j) { return h[size_t(j) * n + i]; }
        float At(int i, int j) const { return h[size_t(std::clamp(j, 0, n - 1)) * n + std::clamp(i, 0, n - 1)]; }

        // Bilinear surface height offset at world (x, z).
        float Sample(float x, float z) const {
            const float fx = (x + halfSize) / Cell() - 0.5f, fz = (z + halfSize) / Cell() - 0.5f;
            const int i = int(std::floor(fx)), j = int(std::floor(fz));
            const float tx = fx - float(i), tz = fz - float(j);
            return (At(i, j) * (1 - tx) + At(i + 1, j) * tx) * (1 - tz) + (At(i, j + 1) * (1 - tx) + At(i + 1, j + 1) * tx) * tz;
        }
        glm::vec3 Normal(float x, float z) const {
            const float e = Cell();
            const float dx = Sample(x + e, z) - Sample(x - e, z), dz = Sample(x, z + e) - Sample(x, z - e);
            return glm::normalize(glm::vec3(-dx, 2.0f * e, -dz));
        }
        double MeanHeight() const { double s = 0.0; for (float x : h) s += x; return s / double(h.size()); }
        float MaxAbs() const { float m = 0.0f; for (float x : h) m = std::max(m, std::abs(x)); return m; }

        // Raised-cosine drop (positive = bump) of `radius` metres at (x, z); the mean is restored so a
        // drop adds no volume (it only starts a wave).
        void AddDrop(float x, float z, float radius, float amount) {
            double added = 0.0;
            for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) {
                const glm::vec2 c = CellCentre(i, j);
                const float d = glm::length(c - glm::vec2(x, z)) / radius;
                if (d >= 1.0f) continue;
                const float a = amount * 0.5f * (1.0f + std::cos(d * PI));
                H(i, j) += a;
                added += a;
            }
            const float mean = float(added / double(h.size()));
            for (float& x2 : h) x2 -= mean;
        }

        void Step() {
            std::vector<float> next(h.size());
            for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) {
                const size_t k = size_t(j) * n + i;
                // reflecting walls: a missing neighbour mirrors the cell itself
                const float l = i > 0 ? h[k - 1] : h[k], r = i + 1 < n ? h[k + 1] : h[k];
                const float d = j > 0 ? h[k - size_t(n)] : h[k], u = j + 1 < n ? h[k + size_t(n)] : h[k];
                v[k] += ((l + r + d + u) * 0.25f - h[k]) * waveK;
                v[k] *= damping;
                next[k] = h[k] + v[k];
            }
            h.swap(next);
        }

        // Free surface around the sphere (mean of 8 samples on a ring of 1.5 radii): the water the
        // ball sits in, without the column it is displacing itself (no feedback).
        float AmbientSurface(const Sphere& s) const {
            float sum = 0.0f;
            for (int k = 0; k < 8; ++k) {
                const float a = float(k) * PI * 0.25f;
                sum += Sample(s.centre.x + std::cos(a) * s.radius * 1.5f, s.centre.z + std::sin(a) * s.radius * 1.5f);
            }
            return level + sum / 8.0f;
        }

        // Column length of the sphere below the ambient surface, per column; the change since the last
        // call is displaced (added to the column so the wave spreads out, the same volume taken evenly
        // from the whole pool so the volume is conserved).
        void CoupleSphere(const Sphere& s) {
            double displaced = 0.0;
            const float r2 = s.radius * s.radius;
            const float surface = AmbientSurface(s);
            for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) {
                const size_t k = size_t(j) * n + i;
                const glm::vec2 c = CellCentre(i, j);
                const float rho2 = (c.x - s.centre.x) * (c.x - s.centre.x) + (c.y - s.centre.z) * (c.y - s.centre.z);
                float now = 0.0f;
                if (rho2 < r2) {
                    const float half = std::sqrt(r2 - rho2);
                    now = std::clamp(surface - (s.centre.y - half), 0.0f, 2.0f * half);
                }
                const float delta = now - occupied[k];
                occupied[k] = now;
                if (delta != 0.0f) { h[k] += delta * 0.5f; displaced += delta * 0.5f; }
            }
            if (displaced != 0.0) {
                const float mean = float(displaced / double(h.size()));
                for (float& x : h) x -= mean;
            }
        }

        // Submerged volume of the sphere below the local free surface (m^3).
        float Submerged(const Sphere& s) const {
            const float surface = AmbientSurface(s);
            return CapVolume(s.radius, surface - (s.centre.y - s.radius));
        }

        // Floor caustics for unit sun direction `toSun` (pointing up, towards the light).
        void ComputeCaustics(const glm::vec3& toSun, const Sphere& s, int supersample = 2) {
            std::fill(caustics.begin(), caustics.end(), 0.0f);
            const int m = n * supersample;
            const float step = 2.0f * halfSize / float(m);
            const glm::vec3 incident = -glm::normalize(toSun);
            const float patch = step * step * std::max(-incident.y, 0.05f);   // light through a flat patch
            const float binArea = Cell() * Cell();
            for (int j = 0; j < m; ++j) for (int i = 0; i < m; ++i) {
                const float x = -halfSize + (float(i) + 0.5f) * step, z = -halfSize + (float(j) + 0.5f) * step;
                const glm::vec3 p(x, level + Sample(x, z), z);
                const glm::vec3 r = Refract(incident, Normal(x, z), 1.0f / WATER_IOR);
                if (r.y >= -1e-4f) continue;
                // blocked by the ball?
                const glm::vec3 oc = p - s.centre;
                const float b = glm::dot(oc, r), c = glm::dot(oc, oc) - s.radius * s.radius;
                if (c < 0.0f || (b < 0.0f && b * b - c > 0.0f)) continue;
                const float t = (floorY - p.y) / r.y;
                glm::vec3 hit = p + r * t;
                // fold rays hitting a wall back into the pool (mirror image keeps the energy)
                for (float* coord : { &hit.x, &hit.z }) {
                    if (*coord > halfSize) *coord = 2.0f * halfSize - *coord;
                    if (*coord < -halfSize) *coord = -2.0f * halfSize - *coord;
                }
                const float fx = (hit.x + halfSize) / Cell() - 0.5f, fz = (hit.z + halfSize) / Cell() - 0.5f;
                const int bi = int(std::floor(fx)), bj = int(std::floor(fz));
                const float tx = fx - float(bi), tz = fz - float(bj);
                const float energy = patch / (binArea * std::max(-incident.y, 0.05f));
                const int is[2] = { bi, bi + 1 }, js[2] = { bj, bj + 1 };
                const float wx[2] = { 1 - tx, tx }, wz[2] = { 1 - tz, tz };
                for (int a = 0; a < 2; ++a) for (int c2 = 0; c2 < 2; ++c2) {
                    const int ii = std::clamp(is[a], 0, n - 1), jj = std::clamp(js[c2], 0, n - 1);
                    caustics[size_t(jj) * n + ii] += energy * wx[a] * wz[c2];
                }
            }
            // light 3x3 blur (the floor is seen through the moving surface anyway)
            std::vector<float> blurred(caustics.size());
            for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) {
                float sum = 0.0f, w = 0.0f;
                for (int dj = -1; dj <= 1; ++dj) for (int di = -1; di <= 1; ++di) {
                    const int ii = i + di, jj = j + dj;
                    if (ii < 0 || jj < 0 || ii >= n || jj >= n) continue;
                    const float k = (di == 0 && dj == 0) ? 4.0f : (di == 0 || dj == 0) ? 2.0f : 1.0f;
                    sum += caustics[size_t(jj) * n + ii] * k;
                    w += k;
                }
                blurred[size_t(j) * n + i] = sum / w;
            }
            caustics.swap(blurred);
        }
    };
}
