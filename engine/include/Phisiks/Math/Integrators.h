#pragma once
// Numerical integrators for simulations, with energy diagnostics for teaching their trade-offs.
//
// Mechanical systems  x'' = a(x):
//   ExplicitEuler     x += v h; v += a(x_old) h       1st order, energy grows (spirals out)
//   SemiImplicitEuler v += a(x) h; x += v_new h       1st order, symplectic: energy bounded
//   VelocityVerlet    x += v h + a h^2/2; v += (a+a')h/2  2nd order, symplectic, time-reversible
// General first-order ODEs  y' = f(t, y):
//   Euler             1st order
//   RK4               4th order, very accurate per step, but not symplectic: energy drifts slowly
//
// State types only need +, - and multiplication by a scalar (float/double, glm vectors, ...).

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <cmath>

namespace Phisiks::Math::Integrate {

    // ---------------------------------------------------------------- second-order (x, v) systems
    template<class Vec, class Accel>
    void ExplicitEuler(Vec& x, Vec& v, double h, Accel acceleration) {
        const Vec a = acceleration(x);
        x = x + v * h;
        v = v + a * h;
    }

    template<class Vec, class Accel>
    void SemiImplicitEuler(Vec& x, Vec& v, double h, Accel acceleration) {
        v = v + acceleration(x) * h;
        x = x + v * h;
    }

    template<class Vec, class Accel>
    void VelocityVerlet(Vec& x, Vec& v, double h, Accel acceleration) {
        const Vec a0 = acceleration(x);
        x = x + v * h + a0 * (0.5 * h * h);
        const Vec a1 = acceleration(x);
        v = v + (a0 + a1) * (0.5 * h);
    }

    // ---------------------------------------------------------------- first-order y' = f(t, y)
    template<class State, class Deriv>
    State Euler(const State& y, double t, double h, Deriv f) {
        return y + f(t, y) * h;
    }

    template<class State, class Deriv>
    State RK4(const State& y, double t, double h, Deriv f) {
        const State k1 = f(t, y);
        const State k2 = f(t + 0.5 * h, y + k1 * (0.5 * h));
        const State k3 = f(t + 0.5 * h, y + k2 * (0.5 * h));
        const State k4 = f(t + h, y + k3 * h);
        return y + (k1 + k2 * 2.0 + k3 * 2.0 + k4) * (h / 6.0);
    }

    // ---------------------------------------------------------------- reference systems
    // Phase-space state (position, velocity) for using RK4 on mechanical systems.
    struct Phase1D {
        double x = 0.0, v = 0.0;
        Phase1D operator+(const Phase1D& o) const { return { x + o.x, v + o.v }; }
        Phase1D operator*(double s) const { return { x * s, v * s }; }
    };

    struct HarmonicOscillator {               // x'' = -(k/m) x
        double k = 1.0, m = 1.0;
        double Acceleration(double x) const { return -(k / m) * x; }
        double Energy(double x, double v) const { return 0.5 * m * v * v + 0.5 * k * x * x; }
        double Period() const { return 2.0 * 3.14159265358979323846 * std::sqrt(m / k); }
    };

    struct Pendulum {                          // theta'' = -(g/L) sin(theta)
        double g = 9.81, length = 1.0;
        double Acceleration(double theta) const { return -(g / length) * std::sin(theta); }
        double Energy(double theta, double omega) const { return 0.5 * length * length * omega * omega + g * length * (1.0 - std::cos(theta)); }   // per unit mass
        double SmallAnglePeriod() const { return 2.0 * 3.14159265358979323846 * std::sqrt(length / g); }
    };

    struct KeplerOrbit {                       // r'' = -GM r / |r|^3
        double gm = 1.0;
        glm::dvec3 Acceleration(const glm::dvec3& r) const {
            const double d = glm::length(r);
            return -gm * r / (d * d * d);
        }
        double Energy(const glm::dvec3& r, const glm::dvec3& v) const { return 0.5 * glm::dot(v, v) - gm / glm::length(r); }   // per unit mass
        double CircularSpeed(double radius) const { return std::sqrt(gm / radius); }
    };
}
