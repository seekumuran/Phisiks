// Headless tests for Phisiks::Math: dual quaternions, rotors, integrators, SDFs.
#include "TestFramework.h"

#include "Phisiks/Math/DualQuat.h"
#include "Phisiks/Math/Integrators.h"
#include "Phisiks/Math/Rotor.h"
#include "Phisiks/Math/SDF.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>

using namespace Phisiks::Math;

namespace {
    bool Near(const glm::vec3& a, const glm::vec3& b, float eps) { return glm::length(a - b) <= eps; }
    const float PI = glm::pi<float>();
}

// ---------------------------------------------------------------------------------- Dual quaternions
TEST_CASE("DualQuat: transforms points like the equivalent matrix") {
    const glm::quat r = glm::angleAxis(0.7f, glm::normalize(glm::vec3(1, 2, 3)));
    const glm::vec3 t(4, -2, 1);
    const DualQuat dq = DualQuat::FromRotationTranslation(r, t);
    const glm::mat4 m = glm::translate(glm::mat4(1), t) * glm::mat4_cast(r);
    const glm::vec3 p(0.3f, -1.2f, 2.5f);
    CHECK(Near(dq.TransformPoint(p), glm::vec3(m * glm::vec4(p, 1)), 1e-5f));
    CHECK(Near(dq.Translation(), t, 1e-5f));
    CHECK(Near(glm::vec3(dq.ToMat4()[3]), t, 1e-5f));
}

TEST_CASE("DualQuat: composition and inverse match matrix algebra") {
    const DualQuat a = DualQuat::FromRotationTranslation(glm::angleAxis(1.1f, glm::vec3(0, 1, 0)), glm::vec3(1, 0, 0));
    const DualQuat b = DualQuat::FromRotationTranslation(glm::angleAxis(-0.4f, glm::vec3(1, 0, 0)), glm::vec3(0, 2, -1));
    const glm::vec3 p(1, 2, 3);
    CHECK(Near((a * b).TransformPoint(p), a.TransformPoint(b.TransformPoint(p)), 1e-5f));
    CHECK(Near((a * a.Inverse()).TransformPoint(p), p, 1e-5f));
}

TEST_CASE("DualQuat: screw decomposition round-trips (Chasles)") {
    // Rotate 90 deg about the vertical line through (2,0,0) while rising 1 m.
    ScrewParameters s;
    s.axis = glm::vec3(0, 1, 0);
    s.moment = glm::cross(glm::vec3(2, 0, 0), s.axis);
    s.angle = PI * 0.5f;
    s.pitch = 1.0f;
    const DualQuat dq = DualQuat::FromScrew(s);
    const ScrewParameters back = dq.ToScrew();
    CHECK_NEAR(back.angle, s.angle, 1e-5);
    CHECK_NEAR(back.pitch, s.pitch, 1e-5);
    CHECK(Near(back.axis, s.axis, 1e-5f));
    CHECK(Near(back.moment, s.moment, 1e-5f));
    CHECK(Near(dq.TransformPoint(glm::vec3(2, 0, 0)), glm::vec3(2, 1, 0), 1e-5f));   // points on the axis only slide
}

TEST_CASE("DualQuat: ScLerp follows the screw (midpoint stays on the circle), endpoints exact") {
    const glm::vec3 pivot(2, 0, 0);
    const auto rotateAboutPivot = [&](float angle) {
        const glm::quat q = glm::angleAxis(angle, glm::vec3(0, 1, 0));
        return DualQuat::FromTranslation(pivot) * DualQuat::FromRotation(q) * DualQuat::FromTranslation(-pivot);
    };
    const DualQuat a = DualQuat::Identity();
    const DualQuat b = rotateAboutPivot(PI * 0.5f);
    const glm::vec3 p(0, 0, 0);   // 2 m from the pivot
    CHECK(Near(DualQuat::ScLerp(a, b, 0.0f).TransformPoint(p), p, 1e-5f));
    CHECK(Near(DualQuat::ScLerp(a, b, 1.0f).TransformPoint(p), b.TransformPoint(p), 1e-4f));
    const glm::vec3 mid = DualQuat::ScLerp(a, b, 0.5f).TransformPoint(p);
    CHECK_NEAR(glm::length(mid - pivot), 2.0, 1e-4);                                  // on the arc, not the chord
    CHECK(Near(mid, rotateAboutPivot(PI * 0.25f).TransformPoint(p), 1e-4f));          // exactly half the screw
    const glm::vec3 chordMid = 0.5f * (p + b.TransformPoint(p));
    CHECK(glm::length(chordMid - pivot) < 1.9f);                                       // linear blend cuts the corner
}

TEST_CASE("DualQuat: blending stays rigid where matrix blending shrinks (candy-wrapper)") {
    // Two bones: identity and a 170 deg twist about X. Blend 50/50 at a skin vertex 1 m off-axis.
    const DualQuat bone0 = DualQuat::Identity();
    const DualQuat bone1 = DualQuat::FromRotation(glm::angleAxis(glm::radians(170.0f), glm::vec3(1, 0, 0)));
    const std::array<DualQuat, 2> bones{ bone0, bone1 };
    const std::array<float, 2> weights{ 0.5f, 0.5f };
    const DualQuat blended = DualQuat::Blend(bones, weights);
    const glm::vec3 vertex(0, 1, 0);
    const float dlbRadius = glm::length(blended.TransformPoint(vertex));
    const glm::mat4 lbs = bone0.ToMat4() * 0.5f + bone1.ToMat4() * 0.5f;
    const float lbsRadius = glm::length(glm::vec3(lbs * glm::vec4(vertex, 1)));
    CHECK_NEAR(dlbRadius, 1.0, 1e-4);   // rigid
    CHECK(lbsRadius < 0.1f);            // linear blend skinning collapses the twisted joint
}

// ---------------------------------------------------------------------------------- Rotors
TEST_CASE("Rotor: plane-angle rotation turns the plane's first axis towards its second") {
    const Rotor3 r = Rotor3::FromPlaneAngle(Bivector3{ 1, 0, 0 }, PI * 0.5f);   // xy plane, 90 deg
    CHECK(Near(r.Rotate(glm::vec3(1, 0, 0)), glm::vec3(0, 1, 0), 1e-5f));
    CHECK(Near(r.Rotate(glm::vec3(0, 0, 1)), glm::vec3(0, 0, 1), 1e-5f));       // normal of the plane is fixed
    CHECK_NEAR(r.Angle(), PI * 0.5, 1e-5);
}

TEST_CASE("Rotor: FromVectors maps from -> to and preserves lengths") {
    const glm::vec3 from = glm::normalize(glm::vec3(1, 2, -0.5f));
    const glm::vec3 to = glm::normalize(glm::vec3(-3, 0.2f, 1));
    const Rotor3 r = Rotor3::FromVectors(from, to);
    CHECK(Near(r.Rotate(from), to, 1e-5f));
    const glm::vec3 v(0.3f, -2.0f, 4.0f);
    CHECK_NEAR(glm::length(r.Rotate(v)), glm::length(v), 1e-5);
    CHECK_NEAR(r.Norm(), 1.0, 1e-6);
}

TEST_CASE("Rotor: geometric product composes rotations; agrees with quaternions") {
    const Rotor3 a = Rotor3::FromPlaneAngle(Bivector3{ 0.3f, -0.5f, 0.8f }, 0.9f);
    const Rotor3 b = Rotor3::FromPlaneAngle(Bivector3{ -1.0f, 0.2f, 0.1f }, 2.1f);
    const glm::vec3 v(1, -2, 0.5f);
    CHECK(Near((b * a).Rotate(v), b.Rotate(a.Rotate(v)), 1e-5f));
    CHECK(Near(a.ToQuat() * v, a.Rotate(v), 1e-5f));
    const Rotor3 back = Rotor3::FromQuat(a.ToQuat());
    CHECK(Near(back.Rotate(v), a.Rotate(v), 1e-6f));
    CHECK(Near((a * a.Reverse()).Rotate(v), v, 1e-5f));
}

TEST_CASE("Rotor: slerp moves at constant angle inside the plane; no gimbal lock") {
    const Rotor3 a = Rotor3::Identity();
    const Rotor3 b = Rotor3::FromPlaneAngle(Bivector3{ 0, 1, 0 }, PI * 0.8f);    // xz plane
    const Rotor3 mid = Rotor3::Slerp(a, b, 0.5f);
    CHECK_NEAR(mid.Angle(), PI * 0.4, 1e-4);
    CHECK(Near(Rotor3::Slerp(a, b, 1.0f).Rotate(glm::vec3(1, 0, 0)), b.Rotate(glm::vec3(1, 0, 0)), 1e-4f));
    // Euler-style "pitch 90 deg" then yaw: still a single well-defined rotation.
    const Rotor3 pitch = Rotor3::FromPlaneAngle(Bivector3{ 0, 0, 1 }, PI * 0.5f);
    const Rotor3 yaw = Rotor3::FromPlaneAngle(Bivector3{ 0, 1, 0 }, 0.3f);
    CHECK_NEAR((yaw * pitch).Norm(), 1.0, 1e-5);
}

// ---------------------------------------------------------------------------------- Integrators
TEST_CASE("Integrators: energy - explicit Euler grows, symplectic methods stay bounded") {
    const Integrate::HarmonicOscillator osc;   // period 2 pi
    const double h = 0.05;
    const int steps = int(100 * osc.Period() / h);   // 100 periods
    const double e0 = osc.Energy(1.0, 0.0);
    auto run = [&](auto stepper) {
        double x = 1.0, v = 0.0, maxError = 0.0;
        for (int i = 0; i < steps; ++i) {
            stepper(x, v);
            maxError = std::max(maxError, std::fabs(osc.Energy(x, v) - e0) / e0);
        }
        return std::pair<double, double>(osc.Energy(x, v) / e0, maxError);
    };
    const auto accel = [&](double x) { return osc.Acceleration(x); };
    const auto euler = run([&](double& x, double& v) { Integrate::ExplicitEuler(x, v, h, accel); });
    const auto symplectic = run([&](double& x, double& v) { Integrate::SemiImplicitEuler(x, v, h, accel); });
    const auto verlet = run([&](double& x, double& v) { Integrate::VelocityVerlet(x, v, h, accel); });
    CHECK(euler.first > 10.0);                 // (1 + h^2)^N blow-up
    CHECK(symplectic.second < 0.05);           // bounded oscillation of size ~h
    CHECK(verlet.second < 0.002);              // bounded, second order
}

TEST_CASE("Integrators: RK4 is 4th order, Euler 1st order (error ratio when halving h)") {
    const auto f = [](double, double y) { return -y; };   // y' = -y, y(1) = e^-1
    auto errorWith = [&](double h, bool rk4) {
        double y = 1.0, t = 0.0;
        const int n = int(std::lround(1.0 / h));
        for (int i = 0; i < n; ++i) { y = rk4 ? Integrate::RK4(y, t, h, f) : Integrate::Euler(y, t, h, f); t += h; }
        return std::fabs(y - std::exp(-1.0));
    };
    const double rkRatio = errorWith(0.1, true) / errorWith(0.05, true);
    const double eulerRatio = errorWith(0.1, false) / errorWith(0.05, false);
    CHECK(rkRatio > 14.0 && rkRatio < 18.0);       // 2^4 = 16
    CHECK(eulerRatio > 1.8 && eulerRatio < 2.2);   // 2^1 = 2
}

TEST_CASE("Integrators: RK4 drifts slowly, Verlet does not drift over long runs") {
    const Integrate::HarmonicOscillator osc;
    const double h = 0.2, e0 = 0.5;
    const int steps = 20000;
    Integrate::Phase1D y{ 1.0, 0.0 };
    const auto deriv = [&](double, const Integrate::Phase1D& s) { return Integrate::Phase1D{ s.v, osc.Acceleration(s.x) }; };
    double x = 1.0, v = 0.0;
    for (int i = 0; i < steps; ++i) {
        y = Integrate::RK4(y, 0.0, h, deriv);
        Integrate::VelocityVerlet(x, v, h, [&](double p) { return osc.Acceleration(p); });
    }
    const double rkDrift = osc.Energy(y.x, y.v) / e0 - 1.0;
    const double verletDrift = osc.Energy(x, v) / e0 - 1.0;
    CHECK(rkDrift < -0.01);                    // RK4 dissipates: energy creeps down monotonically
    CHECK(std::fabs(verletDrift) < 0.02);      // symplectic: no secular drift
}

TEST_CASE("Integrators: Kepler orbit - symplectic keeps the radius, explicit Euler spirals out") {
    const Integrate::KeplerOrbit orbit;
    const double h = 0.01;
    const int steps = int(10 * 2 * 3.14159265358979 / h);   // 10 orbits of radius 1
    auto run = [&](bool symplectic) {
        glm::dvec3 r(1, 0, 0), v(0, orbit.CircularSpeed(1.0), 0);
        double maxRadius = 1.0;
        for (int i = 0; i < steps; ++i) {
            const auto accel = [&](const glm::dvec3& p) { return orbit.Acceleration(p); };
            if (symplectic) Integrate::SemiImplicitEuler(r, v, h, accel);
            else Integrate::ExplicitEuler(r, v, h, accel);
            maxRadius = std::max(maxRadius, glm::length(r));
        }
        return maxRadius;
    };
    CHECK(run(true) < 1.02);
    CHECK(run(false) > 1.3);
}

TEST_CASE("Integrators: small-angle pendulum period matches 2 pi sqrt(L/g)") {
    const Integrate::Pendulum pendulum;
    double theta = 0.05, omega = 0.0, t = 0.0, previous = theta;
    const double h = 1e-4;
    int crossings = 0;
    double firstCrossing = 0.0, thirdCrossing = 0.0;
    while (crossings < 3 && t < 10.0) {
        Integrate::VelocityVerlet(theta, omega, h, [&](double a) { return pendulum.Acceleration(a); });
        t += h;
        if (previous > 0.0 && theta <= 0.0) {   // downward zero crossings, one per period
            ++crossings;
            if (crossings == 1) firstCrossing = t;
            if (crossings == 3) thirdCrossing = t;
        }
        previous = theta;
    }
    CHECK_NEAR((thirdCrossing - firstCrossing) / 2.0, pendulum.SmallAnglePeriod(), 1e-3);
}

// ---------------------------------------------------------------------------------- SDF
TEST_CASE("SDF: primitive distances are exact (inside negative, outside positive)") {
    using namespace SDF;
    CHECK_NEAR(Sphere(glm::vec3(3, 0, 0), 1.0f), 2.0, 1e-6);
    CHECK_NEAR(Sphere(glm::vec3(0), 1.0f), -1.0, 1e-6);
    CHECK_NEAR(Box(glm::vec3(2, 0, 0), glm::vec3(1)), 1.0, 1e-6);
    CHECK_NEAR(Box(glm::vec3(0), glm::vec3(1, 2, 3)), -1.0, 1e-6);
    CHECK_NEAR(Box(glm::vec3(2, 2, 0), glm::vec3(1)), std::sqrt(2.0), 1e-6);   // nearest point is the edge
    CHECK_NEAR(RoundBox(glm::vec3(2, 2, 0), glm::vec3(1), 0.25f), std::sqrt(2.0) * 1.25 - 0.25, 1e-5);   // inner box 0.75, then inflate
    CHECK_NEAR(Capsule(glm::vec3(0, 5, 2), glm::vec3(0, 0, 0), glm::vec3(0, 3, 0), 0.5f), std::sqrt(4.0 + 4.0) - 0.5, 1e-5);
    CHECK_NEAR(Torus(glm::vec3(2, 0, 0), 2.0f, 0.5f), -0.5, 1e-6);
    CHECK_NEAR(Torus(glm::vec3(0, 0, 0), 2.0f, 0.5f), 1.5, 1e-6);
    CHECK_NEAR(Cylinder(glm::vec3(0, 3, 0), 1.0f, 0.5f), 2.0, 1e-6);
    CHECK_NEAR(Cylinder(glm::vec3(2, 0, 0), 1.0f, 0.5f), 1.5, 1e-6);
    CHECK_NEAR(Plane(glm::vec3(0, 3, 0), glm::vec3(0, 1, 0), 1.0f), 2.0, 1e-6);
}

TEST_CASE("SDF: exact fields have unit gradient; normals point outwards") {
    const auto box = [](const glm::vec3& p) { return SDF::Box(p, glm::vec3(1, 0.5f, 2)); };
    const glm::vec3 samples[] = { { 3, 0.2f, 0.1f }, { 0.3f, 2, -1 }, { -2, -3, 4 }, { 0.1f, 0.1f, 3.5f } };
    for (const glm::vec3& p : samples) CHECK_NEAR(glm::length(SDF::Gradient(box, p)), 1.0, 1e-3);
    CHECK(Near(SDF::Normal(box, glm::vec3(1, 0, 0)), glm::vec3(1, 0, 0), 1e-3f));
    const auto sphere = [](const glm::vec3& p) { return SDF::Sphere(p, 2.0f); };
    CHECK(Near(SDF::ClosestPoint(sphere, glm::vec3(0, 7, 0)), glm::vec3(0, 2, 0), 1e-3f));
}

TEST_CASE("SDF: CSG operations and smooth blends") {
    using namespace SDF;
    const float a = 0.3f, b = 0.5f;
    CHECK_NEAR(Union(a, b), 0.3, 1e-7);
    CHECK_NEAR(Intersection(a, b), 0.5, 1e-7);
    CHECK_NEAR(Subtraction(-0.2f, -0.1f), 0.1, 1e-7);   // inside both: removed region is outside the result
    CHECK(SmoothUnion(a, b, 0.4f) < Union(a, b));        // smooth blend adds material in the seam
    CHECK_NEAR(SmoothUnion(0.1f, 2.0f, 0.4f), 0.1, 1e-7); // far apart: identical to the sharp union
    CHECK_NEAR(SmoothUnion(a, b, 0.0f), Union(a, b), 1e-7);
    CHECK(SmoothIntersection(a, b, 0.4f) > Intersection(a, b));
    // Hollow: a sphere minus a smaller sphere is empty at the centre.
    const float hollow = Subtraction(Sphere(glm::vec3(0), 2.0f), Sphere(glm::vec3(0), 1.0f));
    CHECK(hollow > 0.0f);
}

TEST_CASE("SDF: sphere tracing hits analytic surfaces at the exact distance") {
    const auto scene = [](const glm::vec3& p) {
        return SDF::Union(SDF::Sphere(p - glm::vec3(0, 0, 10), 2.0f), SDF::Plane(p, glm::vec3(0, 1, 0), -5.0f));
    };
    const SDF::MarchResult forward = SDF::Raymarch(scene, glm::vec3(0), glm::vec3(0, 0, 1), 100.0f);
    CHECK(forward.hit);
    CHECK_NEAR(forward.distance, 8.0, 1e-3);
    CHECK(Near(forward.normal, glm::vec3(0, 0, -1), 1e-2f));
    const SDF::MarchResult down = SDF::Raymarch(scene, glm::vec3(0), glm::vec3(0, -1, 0), 100.0f);
    CHECK(down.hit);
    CHECK_NEAR(down.distance, 5.0, 1e-3);
    const SDF::MarchResult miss = SDF::Raymarch(scene, glm::vec3(0), glm::vec3(0, 1, 0), 50.0f);
    CHECK(!miss.hit);
}
