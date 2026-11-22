#pragma once
// Accretion-disk particles of showcase 03 (no rendering; unit-tested).
//
// Units of the black-hole mass M (G = c = 1). Particles orbit in the disk plane under the
// Paczynski-Wiita pseudo-Newtonian potential phi = -1 / (r - 2), which reproduces the
// Schwarzschild innermost stable circular orbit (r = 6) and horizon (r = 2); circular speed
// v = sqrt(r) / (r - 2). Each particle is advanced by one of three integrators with the same step:
//   Euler          explicit, first order: the orbital energy grows every step (spirals outwards)
//   Velocity Verlet symplectic, second order: energy error bounded (orbits hold)
//   RK4            classical fourth order: tiny drift at this step size
// In the split-test the disk is split into three 120-degree sectors by starting azimuth, one per
// integrator. Particles leaving the disk (r < 2.5 or r > 32) are re-seeded on a circular orbit.
// Bin() accumulates the polar density (azimuth x radius) the lensing shader samples.
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/common.hpp>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace Showcase::Disk {
    enum class Integrator : int { Euler = 0, Verlet = 1, Rk4 = 2 };
    constexpr double TAU = 6.283185307179586;

    inline glm::dvec2 Acceleration(const glm::dvec2& p) {
        const double r = glm::length(p);
        return -p / r / ((r - 2.0) * (r - 2.0));
    }
    inline double Energy(const glm::dvec2& p, const glm::dvec2& v) { return 0.5 * glm::dot(v, v) - 1.0 / (glm::length(p) - 2.0); }
    inline double CircularSpeed(double r) { return std::sqrt(r) / (r - 2.0); }

    inline void Advance(Integrator method, glm::dvec2& p, glm::dvec2& v, double h) {
        switch (method) {
            case Integrator::Euler: {
                const glm::dvec2 a = Acceleration(p);
                p += v * h;
                v += a * h;
                break;
            }
            case Integrator::Verlet: {
                const glm::dvec2 a = Acceleration(p);
                p += v * h + a * (0.5 * h * h);
                v += (a + Acceleration(p)) * (0.5 * h);
                break;
            }
            case Integrator::Rk4: {
                const glm::dvec2 k1p = v, k1v = Acceleration(p);
                const glm::dvec2 k2p = v + k1v * (0.5 * h), k2v = Acceleration(p + k1p * (0.5 * h));
                const glm::dvec2 k3p = v + k2v * (0.5 * h), k3v = Acceleration(p + k2p * (0.5 * h));
                const glm::dvec2 k4p = v + k3v * h, k4v = Acceleration(p + k3p * h);
                p += (k1p + 2.0 * k2p + 2.0 * k3p + k4p) * (h / 6.0);
                v += (k1v + 2.0 * k2v + 2.0 * k3v + k4v) * (h / 6.0);
                break;
            }
        }
    }

    struct Particle {
        glm::dvec2 p, v;
        double energy0;          // energy when seeded
        uint8_t sector;          // split-test integrator (start azimuth sector)
        float weight;            // brightness weight (clumps / spiral arms)
    };

    class DiskSim {
    public:
        double rIn = 6.0, rOut = 22.0;
        int binsPhi = 256, binsR = 64;
        std::vector<Particle> particles;
        std::vector<float> density;   // binsPhi x binsR RGBA (Euler, Verlet, RK4, total), rows = radius
        uint64_t reseeded[3] = { 0, 0, 0 };

        void Seed(int count, uint32_t seed = 3) {
            rng.seed(seed);
            particles.resize(size_t(count));
            for (Particle& q : particles) Reseed(q, true);
            reseeded[0] = reseeded[1] = reseeded[2] = 0;
        }

        void Step(Integrator method, bool split, double h) {
            for (Particle& q : particles) {
                const Integrator m = split ? Integrator(q.sector) : method;
                Advance(m, q.p, q.v, h);
                const double r = glm::length(q.p);
                if (r < 2.5 || r > 32.0) { ++reseeded[split ? q.sector : int(method)]; Reseed(q, false); }
            }
        }

        // Mean relative energy drift |E - E0| / |E0| per integrator sector.
        glm::dvec3 Drift() const {
            glm::dvec3 sum(0.0), n(0.0);
            for (const Particle& q : particles) {
                sum[q.sector] += std::abs(Energy(q.p, q.v) - q.energy0) / std::abs(q.energy0);
                n[q.sector] += 1.0;
            }
            return sum / glm::max(n, glm::dvec3(1.0));
        }

        // Polar density normalised so a uniform-in-area disk of the seeded profile reads ~1.
        void Bin() {
            density.assign(size_t(binsPhi) * binsR * 4, 0.0f);
            const double dr = (rOut - rIn) / double(binsR);
            for (const Particle& q : particles) {
                const double r = glm::length(q.p);
                if (r < rIn || r >= rOut) continue;
                double phi = std::atan2(q.p.y, q.p.x) / TAU + 0.5;
                const int i = int(phi * binsPhi) % binsPhi, j = int((r - rIn) / dr);
                float* c = &density[(size_t(j) * binsPhi + i) * 4];
                c[q.sector] += q.weight;
                c[3] += q.weight;
            }
            // expected count per bin for the seeded surface density (~ r^-1 per unit area -> uniform in r)
            const float expected = float(particles.size()) / float(binsPhi * binsR);
            for (float& x : density) x /= std::max(expected, 1e-3f);
        }

    private:
        std::mt19937 rng;
        void Reseed(Particle& q, bool initial) {
            std::uniform_real_distribution<double> u(0.0, 1.0);
            const double r = rIn + (rOut - rIn) * std::pow(u(rng), 1.15);
            const double a = u(rng) * TAU;
            const double vr = CircularSpeed(r);
            q.p = glm::dvec2(std::cos(a), std::sin(a)) * r;
            q.v = glm::dvec2(-std::sin(a), std::cos(a)) * vr;   // prograde (counter-clockwise)
            q.energy0 = Energy(q.p, q.v);
            if (initial) q.sector = uint8_t(int(a / TAU * 3.0) % 3);
            // clumps on two spiral arms + hot spots: shear turns them into streaks
            const double arm = std::cos(2.0 * a - 1.6 * std::log(r)) * 0.5 + 0.5;
            q.weight = float(0.45 + 0.9 * arm * arm + (u(rng) < 0.03 ? 2.5 : 0.0));
        }
    };
}
