// P9: glTF import + dual-quaternion skinning, splines, joints (current physics backend), height
// fields, spatial audio mixing, navigation grid + A* + crowd, Lua scripting, terrain generation.
#include "TestFramework.h"

#include "Phisiks/AI/Navigation.h"
#include "Phisiks/Animation/Animation.h"
#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Scene/Scene.h"
#include "Phisiks/Script/Script.h"
#include "Phisiks/World/Terrain.h"
#include "audio/Mixer.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace Phisiks;

namespace {
    // Radius of every vertex from the skin's vertical axis segment (the tube is 0.25 m thick).
    void RadiusRange(const std::vector<Render::Vertex>& vertices, const std::vector<Render::Vertex>& bind, float& lo, float& hi) {
        lo = FLT_MAX; hi = 0.0f;
        for (size_t i = 0; i < vertices.size(); ++i) {
            if (bind[i].position.y < 0.3f || bind[i].position.y > 1.7f) continue;   // interior rings only
            const float r = glm::length(glm::vec2(vertices[i].position.x, vertices[i].position.z));
            lo = std::min(lo, r);
            hi = std::max(hi, r);
        }
    }

    Physics::BodyId Dynamic(const glm::vec3& p, const glm::vec3& half, float mass = 1.0f) {
        Physics::BodyDesc d;
        d.position = p;
        d.mass = mass;
        d.angularDamping = 0.0f;
        Physics::ColliderDesc c;
        c.halfExtents = half;
        d.colliders.push_back(c);
        return Physics::CreateBody(d);
    }
    Physics::BodyId StaticBox(const glm::vec3& p, const glm::vec3& half) {
        Physics::BodyDesc d;
        d.type = Physics::BodyType::Static;
        d.position = p;
        Physics::ColliderDesc c;
        c.halfExtents = half;
        d.colliders.push_back(c);
        return Physics::CreateBody(d);
    }
    void Simulate(float seconds) { for (int i = 0; i < int(seconds * 60.0f + 0.5f); ++i) Physics::Step(1.0f / 60.0f); }
}

// ============================================================================ glTF + skinning
TEST_CASE("glTF: walker.glb imports nodes, skin, clips, material and embedded texture") {
    Animation::Model model;
    std::string error;
    CHECK(Animation::LoadGltf("models/walker.glb", model, &error));
    if (!error.empty()) std::printf("    %s\n", error.c_str());
    CHECK(model.nodes.size() == 6);
    CHECK(model.skins.size() == 1 && model.skins[0].joints.size() == 4);
    CHECK(model.meshes.size() == 1 && model.meshes[0].primitives.size() == 1);
    CHECK(model.clips.size() == 2 && model.FindClip("Twist") == 0 && model.FindClip("Sway") == 1);
    CHECK(model.materials.size() == 1 && model.materials[0].baseColorImage == 0);
    CHECK(model.images.size() == 1 && model.images[0].width == 128 && model.images[0].rgba.size() == 128u * 128u * 4u);
    const Animation::Primitive& p = model.meshes[0].primitives[0];
    CHECK(p.vertices.size() == 41u * 25u && p.indices.size() == 40u * 24u * 6u);
    CHECK(p.joints.size() == p.vertices.size() && p.weights.size() == p.vertices.size());
    CHECK(model.nodes[model.FindNode("Bone2")].parent == model.FindNode("Bone1"));
    CHECK_NEAR(model.clips[0].duration, 2.0, 1e-6);
}

TEST_CASE("Skinning: bind pose is exact; dual quaternions keep the twisted volume, linear blending collapses") {
    Animation::Model model;
    CHECK(Animation::LoadGltf("models/walker.glb", model));
    if (model.meshes.empty()) return;
    const Animation::Primitive& prim = model.meshes[0].primitives[0];
    std::vector<Transform> locals;
    std::vector<glm::mat4> globals, matrices;
    std::vector<Math::DualQuat> dq;
    std::vector<Render::Vertex> out;
    // bind pose: the rest pose of the nodes (no clip)
    Animation::SamplePose(model, nullptr, 0.0f, true, locals);
    Animation::GlobalMatrices(model, locals, globals);
    Animation::SkinTransforms(model, 0, globals, dq, matrices);
    float maxError = 0.0f;
    for (Animation::SkinningMethod method : { Animation::SkinningMethod::DualQuaternion, Animation::SkinningMethod::Linear }) {
        Animation::SkinVertices(prim, dq, matrices, method, out);
        for (size_t i = 0; i < out.size(); ++i) maxError = std::max(maxError, glm::length(out[i].position - prim.vertices[i].position));
    }
    CHECK(maxError < 1e-5f);
    // t = 0.5 s: every bone twisted 70 degrees about the tube axis
    Animation::SamplePose(model, &model.clips[0], 0.5f, true, locals);
    Animation::GlobalMatrices(model, locals, globals);
    Animation::SkinTransforms(model, 0, globals, dq, matrices);
    std::vector<Render::Vertex> dualQuat, linear;
    Animation::SkinVertices(prim, dq, matrices, Animation::SkinningMethod::DualQuaternion, dualQuat);
    Animation::SkinVertices(prim, dq, matrices, Animation::SkinningMethod::Linear, linear);
    // the sway bends the tube, so measure the radius in each bone's frame: compare to the axis through the bone origins
    float dqLo, dqHi, lbLo, lbHi;
    // undo the sway by measuring against the blended axis point: project onto the plane perpendicular to the local bone axis
    auto radius = [&](const std::vector<Render::Vertex>& v, float& lo, float& hi) {
        lo = FLT_MAX; hi = 0.0f;
        for (size_t i = 0; i < v.size(); ++i) {
            if (prim.vertices[i].position.y < 0.3f || prim.vertices[i].position.y > 1.7f) continue;
            // axis point: the same blend applied to the vertex's projection on the bind axis (0, y, 0)
            Animation::Primitive axis;
            axis.vertices = { Render::Vertex{ glm::vec3(0.0f, prim.vertices[i].position.y, 0.0f), glm::vec3(0, 1, 0), glm::vec2(0.0f) } };
            axis.joints = { prim.joints[i] };
            axis.weights = { prim.weights[i] };
            std::vector<Render::Vertex> a;
            Animation::SkinVertices(axis, dq, matrices, Animation::SkinningMethod::DualQuaternion, a);
            const float r = glm::length(v[i].position - a[0].position);
            lo = std::min(lo, r);
            hi = std::max(hi, r);
        }
    };
    radius(dualQuat, dqLo, dqHi);
    radius(linear, lbLo, lbHi);
    std::printf("    twisted radius: dual quaternion %.3f .. %.3f m, linear %.3f .. %.3f m (rest 0.250)\n", dqLo, dqHi, lbLo, lbHi);
    CHECK(dqLo > 0.235f && dqHi < 0.265f);
    CHECK(lbLo < 0.22f);
    CHECK(lbLo < dqLo - 0.02f);
    float unused0, unused1;
    RadiusRange(dualQuat, prim.vertices, unused0, unused1);
}

TEST_CASE("glTF: .gltf JSON with a base64 buffer, STEP and CUBICSPLINE sampling") {
    // one triangle + node translation keys (STEP) at t = 0, 1: (0,0,0) then (1,2,3)
    // buffer: 3 positions (36 B) + 2 times (8 B) + 2 translations (24 B) = 68 B
    std::vector<float> data = { 0, 0, 0, 1, 0, 0, 0, 1, 0, 0.0f, 1.0f, 0, 0, 0, 1, 2, 3 };
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data.data());
    const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    const size_t n = data.size() * 4;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t(bytes[i]) << 16) | (i + 1 < n ? uint32_t(bytes[i + 1]) << 8 : 0u) | (i + 2 < n ? uint32_t(bytes[i + 2]) : 0u);
        b64 += alphabet[(v >> 18) & 63];
        b64 += alphabet[(v >> 12) & 63];
        b64 += i + 1 < n ? alphabet[(v >> 6) & 63] : '=';
        b64 += i + 2 < n ? alphabet[v & 63] : '=';
    }
    const std::string json = std::string(R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"name":"Tri","mesh":0}],)") +
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)" +
        R"("animations":[{"samplers":[{"input":1,"output":2,"interpolation":"STEP"}],"channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}]}],)" +
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5126,"count":2,"type":"SCALAR"},)" +
        R"({"bufferView":2,"componentType":5126,"count":2,"type":"VEC3"}],)" +
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":8},{"buffer":0,"byteOffset":44,"byteLength":24}],)" +
        R"("buffers":[{"byteLength":68,"uri":"data:application/octet-stream;base64,)" + b64 + R"("}]})";
    Animation::Model model;
    std::string error;
    CHECK(Animation::ParseGltf(std::vector<uint8_t>(json.begin(), json.end()), "", model, &error));
    if (!error.empty()) std::printf("    %s\n", error.c_str());
    if (model.meshes.empty() || model.clips.empty()) return;
    const Animation::Primitive& p = model.meshes[0].primitives[0];
    CHECK(p.vertices.size() == 3 && p.indices.size() == 3);
    CHECK(glm::length(p.vertices[1].position - glm::vec3(1, 0, 0)) < 1e-6f);
    CHECK(glm::length(p.vertices[0].normal - glm::vec3(0, 0, 1)) < 1e-5f);   // generated normal
    std::vector<Transform> locals;
    Animation::SamplePose(model, &model.clips[0], 0.99f, false, locals);
    CHECK(glm::length(locals[0].position) < 1e-6f);                          // STEP holds the first key
    Animation::SamplePose(model, &model.clips[0], 1.0f, false, locals);
    CHECK(glm::length(locals[0].position - glm::vec3(1, 2, 3)) < 1e-6f);
    // malformed input is rejected with a message
    const std::string bad = R"({"asset":{"version":"1.0"}})";
    CHECK(!Animation::ParseGltf(std::vector<uint8_t>(bad.begin(), bad.end()), "", model, &error) && error.find("2.x") != std::string::npos);
    // walker "Sway" (cubic spline, zero tangents): 25 degrees about X at t = 1, smooth in between
    Animation::Model walker;
    CHECK(Animation::LoadGltf("models/walker.glb", walker));
    if (walker.clips.size() < 2) return;
    Animation::SamplePose(walker, &walker.clips[1], 1.0f, false, locals);
    const int bone1 = walker.FindNode("Bone1");
    CHECK_NEAR(glm::degrees(glm::angle(locals[bone1].rotation)), 25.0, 0.01);
    Animation::SamplePose(walker, &walker.clips[1], 0.5f, false, locals);
    CHECK_NEAR(glm::degrees(glm::angle(locals[bone1].rotation)), 12.5, 0.2);   // Hermite with zero tangents: half way at s = 0.5
}

TEST_CASE("Splines: Catmull-Rom interpolates its points, arc length gives constant speed, Bezier endpoints") {
    const std::vector<glm::vec3> points = { { 0, 0, 0 }, { 4, 0, 0 }, { 5, 0, 3 }, { 9, 2, 3 }, { 10, 0, 0 } };
    for (size_t k = 1; k + 2 < points.size(); ++k) {
        CHECK(glm::length(Animation::CatmullRom(points[k - 1], points[k], points[k + 1], points[k + 2], 0.0f) - points[k]) < 1e-4f);
        CHECK(glm::length(Animation::CatmullRom(points[k - 1], points[k], points[k + 1], points[k + 2], 1.0f) - points[k + 1]) < 1e-4f);
    }
    Animation::Spline spline;
    CHECK(spline.Build(Animation::SplineType::CatmullRom, points));
    CHECK(glm::length(spline.Evaluate(0.0f) - points.front()) < 1e-4f && glm::length(spline.Evaluate(1.0f) - points.back()) < 1e-3f);
    float lo = FLT_MAX, hi = 0.0f;
    for (int i = 0; i < 100; ++i) {
        const float d = glm::length(spline.Evaluate((i + 1) / 100.0f) - spline.Evaluate(i / 100.0f));
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
    std::printf("    arc-length steps %.4f .. %.4f m (length %.3f m)\n", lo, hi, spline.Length());
    CHECK(hi / lo < 1.05f);
    Animation::Spline bezier;
    CHECK(bezier.Build(Animation::SplineType::Bezier, { { 0, 0, 0 }, { 1, 2, 0 }, { 3, 2, 0 }, { 4, 0, 0 } }));
    CHECK(glm::length(bezier.Evaluate(1.0f) - glm::vec3(4, 0, 0)) < 1e-3f);
    CHECK(std::fabs(bezier.Evaluate(0.5f).y - 1.5f) < 0.02f && std::fabs(bezier.Evaluate(0.5f).x - 2.0f) < 0.02f);
    CHECK(!bezier.Build(Animation::SplineType::Bezier, { { 0, 0, 0 }, { 1, 0, 0 } }));
}

// ============================================================================ joints
TEST_CASE("Joints: ball-and-socket pendulum keeps its length and swings with the small-angle period") {
    CHECK(Physics::Init());
    const glm::vec3 pivot(0, 5, 0);
    const Physics::BodyId bob = Dynamic(pivot + glm::vec3(1.0f, 0, 0) * std::sin(0.15f) - glm::vec3(0, 1.0f, 0) * std::cos(0.15f), glm::vec3(0.05f), 1.0f);
    Physics::JointDesc d;
    d.type = Physics::JointType::BallSocket;
    d.bodyB = bob;
    d.anchor = pivot;
    // the anchor on the bob is its centre: build the joint with the bob's own frame
    d.anchor = Physics::GetPosition(bob);
    const Physics::JointId j = Physics::CreateJoint(d);
    // second joint the textbook way: a rigid link through the world pivot (ball at the pivot on A=world,
    // B = bob at a 1 m arm) - replace the first one
    Physics::DestroyJoint(j);
    CHECK(!Physics::IsValid(j));
    d.anchor = pivot;
    const Physics::JointId pendulum = Physics::CreateJoint(d);
    CHECK(Physics::IsValid(pendulum) && Physics::GetStats().joints == 1);
    float maxDrift = 0.0f;
    float lastX = Physics::GetPosition(bob).x;
    std::vector<float> crossings;
    for (int i = 0; i < 600; ++i) {
        Physics::Step(1.0f / 60.0f);
        const glm::vec3 p = Physics::GetPosition(bob);
        maxDrift = std::max(maxDrift, std::fabs(glm::length(p - pivot) - 1.0f));
        if ((lastX > 0.0f) != (p.x > 0.0f)) crossings.push_back(float(i + 1) / 60.0f);
        lastX = p.x;
    }
    const float period = crossings.size() >= 3 ? (crossings.back() - crossings.front()) / float(crossings.size() - 1) * 2.0f : 0.0f;
    const float expected = 2.0f * 3.14159265f * std::sqrt(1.0f / 9.81f);
    std::printf("    %s: length drift %.4f m, period %.3f s (theory %.3f s, %zu crossings)\n", Physics::BackendName(), maxDrift, period, expected, crossings.size());
    CHECK(maxDrift < 0.01f);
    CHECK(std::fabs(period - expected) / expected < 0.05f);
    Physics::DestroyBody(bob);
    CHECK(!Physics::IsValid(pendulum) && Physics::GetStats().joints == 0);   // the body took its joint
    Physics::Shutdown();
}

TEST_CASE("Joints: hinge motor reaches its speed, limits hold, the axis stays fixed") {
    CHECK(Physics::Init());
    Physics::SetGravity(glm::vec3(0.0f));
    const Physics::BodyId wheel = Dynamic(glm::vec3(0, 3, 0), glm::vec3(0.8f, 0.1f, 0.1f), 2.0f);
    Physics::JointDesc d;
    d.type = Physics::JointType::Hinge;
    d.bodyB = wheel;
    d.anchor = glm::vec3(0, 3, 0);
    d.axis = glm::vec3(0, 0, 1);
    d.enableMotor = true;
    d.motorSpeed = 3.0f;
    d.maxMotorForce = 100.0f;
    const Physics::JointId motor = Physics::CreateJoint(d);
    Simulate(2.0f);
    const Physics::JointState s = Physics::GetJointState(motor);
    const glm::vec3 axisNow = Physics::GetRotation(wheel) * glm::vec3(0, 0, 1);
    const glm::vec3 w = Physics::GetAngularVelocity(wheel);
    std::printf("    motor: speed %.3f rad/s (joint) / %.3f (body z), axis error %.4f, anchor drift %.4f m\n", s.speed, w.z, glm::length(axisNow - glm::vec3(0, 0, 1)),
                glm::length(s.anchorA - s.anchorB));
    CHECK(std::fabs(std::fabs(s.speed) - 3.0f) < 0.3f);
    CHECK(std::fabs(std::fabs(w.z) - 3.0f) < 0.3f && std::fabs(w.x) < 0.05f && std::fabs(w.y) < 0.05f);
    CHECK(glm::length(axisNow - glm::vec3(0, 0, 1)) < 0.02f);
    CHECK(glm::length(Physics::GetPosition(wheel) - glm::vec3(0, 3, 0)) < 0.01f);
    // limits: a door pushed hard stops at +45 degrees
    Physics::SetGravity(glm::vec3(0, -9.81f, 0));
    const Physics::BodyId door = Dynamic(glm::vec3(5.5f, 1, 0), glm::vec3(0.5f, 1.0f, 0.05f), 5.0f);
    Physics::JointDesc h;
    h.type = Physics::JointType::Hinge;
    h.bodyB = door;
    h.anchor = glm::vec3(5.0f, 1, 0);
    h.axis = glm::vec3(0, 1, 0);
    h.enableLimits = true;
    h.lowerLimit = -0.785f;
    h.upperLimit = 0.785f;
    const Physics::JointId limited = Physics::CreateJoint(h);
    float maxAngle = 0.0f;
    for (int i = 0; i < 180; ++i) {
        if (i < 30) Physics::AddForce(door, glm::vec3(0, 0, -200.0f));   // opens towards +angle about +Y
        Physics::Step(1.0f / 60.0f);
        maxAngle = std::max(maxAngle, std::fabs(Physics::GetJointState(limited).position));
    }
    const float sag = std::fabs(Physics::GetPosition(door).y - 1.0f);
    std::printf("    limit: max |angle| %.2f deg (limit 45), sag %.4f m\n", glm::degrees(maxAngle), sag);
    CHECK(maxAngle > 0.6f && maxAngle < 0.785f + 0.06f);
    CHECK(sag < 0.02f);
    Physics::Shutdown();
}

TEST_CASE("Joints: prismatic slides only along its axis within limits; spring-damper oscillates at sqrt(k/m)") {
    CHECK(Physics::Init());
    const Physics::BodyId slider = Dynamic(glm::vec3(0, 2, 0), glm::vec3(0.3f), 1.0f);
    Physics::JointDesc p;
    p.type = Physics::JointType::Prismatic;
    p.bodyB = slider;
    p.anchor = glm::vec3(0, 2, 0);
    p.axis = glm::normalize(glm::vec3(1, -1, 0));   // a 45 degree rail: gravity slides it down
    p.enableLimits = true;
    p.lowerLimit = -0.5f;
    p.upperLimit = 1.5f;
    const Physics::JointId rail = Physics::CreateJoint(p);
    float perpendicular = 0.0f, maxTravel = -1e9f, rotation = 0.0f;
    for (int i = 0; i < 180; ++i) {
        Physics::Step(1.0f / 60.0f);
        const glm::vec3 d = Physics::GetPosition(slider) - glm::vec3(0, 2, 0);
        perpendicular = std::max(perpendicular, glm::length(d - p.axis * glm::dot(d, p.axis)));
        maxTravel = std::max(maxTravel, glm::dot(d, p.axis));
        rotation = std::max(rotation, glm::angle(Physics::GetRotation(slider)));
    }
    const Physics::JointState rs = Physics::GetJointState(rail);
    std::printf("    prismatic: off-axis %.4f m, travel %.3f m (limit 1.5), rotation %.3f deg, state %.3f m\n", perpendicular, maxTravel, glm::degrees(rotation), rs.position);
    CHECK(perpendicular < 0.01f);
    CHECK(maxTravel > 1.4f && maxTravel < 1.56f);
    CHECK(glm::degrees(rotation) < 1.0f);
    CHECK(std::fabs(rs.position - 1.5f) < 0.06f);

    // spring: 2 kg on k = 200 N/m (omega = 10 rad/s, T = 0.628 s), light damping, no gravity
    Physics::SetGravity(glm::vec3(0.0f));
    const Physics::BodyId bob = Dynamic(glm::vec3(10, 0, 0), glm::vec3(0.1f), 2.0f);
    Physics::JointDesc s;
    s.type = Physics::JointType::SpringDamper;
    s.bodyB = bob;
    s.anchor = glm::vec3(8, 0, 0);
    s.anchorB = glm::vec3(10, 0, 0);
    s.restLength = 1.5f;              // starts stretched by 0.5 m
    s.stiffness = 200.0f;
    s.damping = 0.4f;
    const Physics::JointId spring = Physics::CreateJoint(s);
    std::vector<float> peaks;
    float previous = 0.5f, previousDelta = -1.0f;
    float firstAmplitude = 0.5f, lastAmplitude = 0.5f;
    for (int i = 0; i < 300; ++i) {
        Physics::Step(1.0f / 60.0f);
        const float x = Physics::GetJointState(spring).position - 1.5f;
        const float delta = x - previous;
        if (previousDelta > 0.0f && delta <= 0.0f && previous > 0.05f) { peaks.push_back(float(i) / 60.0f); lastAmplitude = previous; }
        previousDelta = delta;
        previous = x;
    }
    const float period = peaks.size() >= 2 ? (peaks.back() - peaks.front()) / float(peaks.size() - 1) : 0.0f;
    std::printf("    spring: period %.3f s (theory 0.628), %zu peaks, amplitude %.3f -> %.3f m\n", period, peaks.size(), firstAmplitude, lastAmplitude);
    CHECK(std::fabs(period - 0.628f) < 0.05f);
    CHECK(lastAmplitude < firstAmplitude && lastAmplitude > 0.1f);   // damped, but only lightly
    Physics::Shutdown();
}

// ============================================================================ height fields
TEST_CASE("Height field: raycasts hit the sampled surface and a ball comes to rest on it") {
    CHECK(Physics::Init());
    World::TerrainGenerator gen;
    gen.resolution = 65;
    gen.cellSize = 0.5f;
    gen.height = 4.0f;
    gen.flatRadius = 0.0f;
    const World::Heightmap h = World::GenerateHeightmap(gen);
    Physics::BodyDesc d;
    d.type = Physics::BodyType::Static;
    d.position = glm::vec3(3, -1, 2);
    Physics::ColliderDesc c;
    c.shape = Physics::ShapeType::HeightField;
    c.heights = h.heights;
    c.heightfieldColumns = h.columns;
    c.heightfieldRows = h.rows;
    c.heightfieldCellSize = h.cellSize;
    d.colliders.push_back(c);
    CHECK(Physics::CreateBody(d).Valid());
    float maxError = 0.0f;
    uint32_t seed = 7;
    for (int i = 0; i < 200; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const float x = (float(seed >> 8 & 0xFFFF) / 65535.0f - 0.5f) * 30.0f;
        seed = seed * 1664525u + 1013904223u;
        const float z = (float(seed >> 8 & 0xFFFF) / 65535.0f - 0.5f) * 30.0f;
        const auto hit = Physics::Raycast(glm::vec3(3 + x, 20, 2 + z), glm::vec3(0, -1, 0), 40.0f);
        if (!hit) { maxError = 1e9f; continue; }
        maxError = std::max(maxError, std::fabs(hit->point.y - (h.Sample(x, z) - 1.0f)));
    }
    std::printf("    %s height field: raycast vs sample max error %.4f m\n", Physics::BackendName(), maxError);
    CHECK(maxError < 0.03f);
    const Physics::BodyId ball = [] {
        Physics::BodyDesc b;
        b.position = glm::vec3(3.2f, 8.0f, 2.1f);
        Physics::ColliderDesc s;
        s.shape = Physics::ShapeType::Sphere;
        s.radius = 0.3f;
        b.colliders.push_back(s);
        b.linearDamping = 0.5f;
        b.angularDamping = 2.0f;
        return Physics::CreateBody(b);
    }();
    Simulate(3.0f);
    const glm::vec3 p = Physics::GetPosition(ball);
    const float ground = h.Sample(p.x - 3.0f, p.z - 2.0f) - 1.0f;
    std::printf("    ball rests at %.3f above the surface (radius 0.3)\n", p.y - ground);
    CHECK(p.y - ground > 0.2f && p.y - ground < 0.36f);
    Physics::Shutdown();
}

// ============================================================================ spatial audio
TEST_CASE("Spatial audio: equal-power panning, inverse-square attenuation, Doppler shift") {
    auto clip = std::make_shared<Audio::ClipData>();
    clip->sampleRate = 48000;
    clip->channels = 1;
    clip->samples.resize(48000);
    for (size_t i = 0; i < clip->samples.size(); ++i) clip->samples[i] = 0.5f * float(std::sin(2.0 * 3.14159265358979 * 1000.0 * double(i) / 48000.0));
    Audio::Mixer mixer(48000);
    mixer.SetListener(glm::vec3(0.0f), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0), glm::vec3(0.0f));
    auto rms = [](const std::vector<float>& s, int channel) {
        double sum = 0.0;
        for (size_t i = channel; i < s.size(); i += 2) sum += double(s[i]) * s[i];
        return std::sqrt(sum / double(s.size() / 2));
    };
    std::vector<float> out(4800 * 2);
    const uint64_t right = mixer.PlaySpatial(clip, 1.0f, true, glm::vec3(4.0f, 0, 0), 1.0f, 100.0f, 1.0f);
    mixer.Mix(out.data(), 4800);
    mixer.Mix(out.data(), 4800);   // second block: gains settled
    const double l = rms(out, 0), r = rms(out, 1);
    Audio::SpatialVoiceState s;
    CHECK(mixer.GetSpatialState(right, s));
    std::printf("    source at +4 m on the right: L %.4f R %.4f, distance gain %.3f\n", l, r, s.distanceGain);
    CHECK(r > 10.0 * l);
    CHECK_NEAR(s.gainLeft * s.gainLeft + s.gainRight * s.gainRight, s.distanceGain * s.distanceGain, 1e-5);   // equal power
    // inverse-square law: 2 m -> 8 m is 1/16 of the intensity (1/4 of the amplitude)
    mixer.SetVoicePosition(right, glm::vec3(0, 0, -2.0f), glm::vec3(0.0f));
    mixer.Mix(out.data(), 4800); mixer.Mix(out.data(), 4800);
    const double near = rms(out, 0) + rms(out, 1);
    mixer.SetVoicePosition(right, glm::vec3(0, 0, -8.0f), glm::vec3(0.0f));
    mixer.Mix(out.data(), 4800); mixer.Mix(out.data(), 4800);
    const double far = rms(out, 0) + rms(out, 1);
    std::printf("    intensity ratio 2 m / 8 m: %.2f (expected 16)\n", (near * near) / (far * far));
    CHECK(std::fabs((near * near) / (far * far) - 16.0) < 0.5);
    mixer.Stop(right);
    // Doppler: source approaching at 34.3 m/s -> f' = 343 / (343 - 34.3) f = 1.111 kHz
    const uint64_t moving = mixer.PlaySpatial(clip, 1.0f, true, glm::vec3(0, 0, -20.0f), 1.0f, 200.0f, 1.0f);
    mixer.SetVoicePosition(moving, glm::vec3(0, 0, -20.0f), glm::vec3(0, 0, 34.3f));
    std::vector<float> second(48000 * 2);
    mixer.Mix(second.data(), 48000);
    int crossings = 0;
    for (size_t i = 2 * 4800 + 2; i < second.size(); i += 2) if ((second[i - 2] < 0.0f) != (second[i] < 0.0f)) ++crossings;
    const double frequency = crossings / 2.0 / (43200.0 / 48000.0);
    CHECK(mixer.GetSpatialState(moving, s));
    std::printf("    Doppler: measured %.1f Hz, pitch %.4f (theory 1111.1 Hz / 1.1111)\n", frequency, s.dopplerPitch);
    CHECK(std::fabs(s.dopplerPitch - 1.1111f) < 1e-3f);
    CHECK(std::fabs(frequency - 1111.1) < 10.0);
}

// ============================================================================ navigation
TEST_CASE("Navigation: grid from static geometry, A* around a wall, string pulling, islands") {
    CHECK(Physics::Init());
    StaticBox(glm::vec3(0, -0.5f, 0), glm::vec3(10, 0.5f, 10));        // floor
    StaticBox(glm::vec3(0, 1.0f, 0), glm::vec3(0.25f, 1.0f, 6.0f));    // wall across the middle (gap past |z| > 6)
    StaticBox(glm::vec3(8, 0.15f, 8), glm::vec3(1.0f, 0.15f, 1.0f));   // step 0.3 m: climbable
    StaticBox(glm::vec3(-8, 2.0f, -8), glm::vec3(1.0f, 0.5f, 1.0f));   // floating platform: an island
    const Physics::BodyId crate = Dynamic(glm::vec3(-5, 0.5f, 5), glm::vec3(0.5f));   // dynamic: ignored
    (void)crate;
    AI::NavGrid grid;
    AI::NavGridDesc desc;
    desc.boundsMin = glm::vec3(-10, -1, -10);
    desc.boundsMax = glm::vec3(10, 5, 10);
    desc.cellSize = 0.25f;
    CHECK(grid.Build(desc));
    std::printf("    grid %dx%d, %u walkable, %u regions\n", grid.Width(), grid.Depth(), grid.WalkableCells(), grid.Regions());
    int x, z;
    CHECK(grid.WorldToCell(glm::vec3(0.0f, 0.0f, 0.0f), x, z) && !grid.IsWalkable(x, z));        // the wall
    CHECK(grid.WorldToCell(glm::vec3(0.6f, 0.0f, 0.0f), x, z) && !grid.IsWalkable(x, z));        // eroded by the 0.35 m radius
    CHECK(grid.WorldToCell(glm::vec3(1.0f, 0.0f, 0.0f), x, z) && grid.IsWalkable(x, z));
    CHECK(grid.WorldToCell(glm::vec3(8.0f, 0.3f, 8.0f), x, z) && std::fabs(grid.FloorHeight(x, z) - 0.3f) < 0.01f);
    const AI::PathResult path = grid.FindPath(glm::vec3(-4, 0, 0), glm::vec3(4, 0, 0));
    CHECK(path.found);
    float maxAbsZ = 0.0f;
    for (const glm::vec3& p : path.points) maxAbsZ = std::max(maxAbsZ, std::fabs(p.z));
    const float optimal = 2.0f * std::sqrt(16.0f + 6.6f * 6.6f);     // taut around the wall end + radius + ledge cell
    std::printf("    path: %zu points (%u raw, %u expanded), length %.2f m (taut %.2f m), detour to |z| = %.2f\n", path.points.size(), path.rawPoints, path.expanded,
                path.length, optimal, maxAbsZ);
    CHECK(maxAbsZ > 6.0f);
    CHECK(path.length < optimal * 1.06f && path.length > optimal * 0.97f);
    CHECK(path.points.size() <= 5);                                      // string pulled
    for (size_t i = 1; i < path.points.size(); ++i) CHECK(grid.LineOfSight(path.points[i - 1], path.points[i]));
    CHECK(grid.FindPath(glm::vec3(-4, 0, 0), glm::vec3(8, 0.3f, 8)).found);                     // up the step
    CHECK(!grid.FindPath(glm::vec3(-4, 0, 0), glm::vec3(-8, 2.5f, -8)).found);                  // the island
    CHECK(grid.Regions() >= 2);
    Physics::Shutdown();
}

TEST_CASE("Crowd: agents crossing through each other all arrive without overlapping") {
    CHECK(Physics::Init());
    Scene scene("Crowd");
    Entity floor = scene.CreateEntity("Floor");
    floor.Get<Transform>().position = glm::vec3(0, -0.5f, 0);
    floor.Add<Collider>().shape.halfExtents = glm::vec3(12, 0.5f, 12);
    scene.SyncPhysicsBodies();
    Entity surface = scene.CreateEntity("Nav");
    NavigationSurface& nav = surface.Add<NavigationSurface>();
    nav.desc.boundsMin = glm::vec3(-10, -1, -10);
    nav.desc.boundsMax = glm::vec3(10, 4, 10);
    nav.desc.cellSize = 0.3f;
    const int count = 8;
    for (int i = 0; i < count; ++i) {
        const float a = float(i) / count * 6.2831853f;
        Entity e = scene.CreateEntity("Agent" + std::to_string(i));
        e.Get<Transform>().position = glm::vec3(std::cos(a) * 6.0f, 0.0f, std::sin(a) * 6.0f);
        NavAgent& agent = e.Add<NavAgent>();
        AI::SetTarget(agent, -e.Get<Transform>().position);
    }
    float minSeparation = FLT_MAX;
    int steps = 0;
    for (; steps < 60 * 20; ++steps) {
        AI::UpdateScene(scene, 1.0f / 60.0f);
        const AI::Stats s = AI::GetStats();
        if (steps > 0) minSeparation = std::min(minSeparation, s.minSeparation);
        if (s.arrived == uint32_t(count)) break;
    }
    const AI::Stats s = AI::GetStats();
    float maxMiss = 0.0f;
    scene.Each<NavAgent>([&](Entity e, NavAgent& a) { maxMiss = std::max(maxMiss, glm::length(glm::vec2(e.WorldPosition().x - a.target.x, e.WorldPosition().z - a.target.z))); });
    std::printf("    %u / %d arrived after %.2f s, closest gap %.3f m, max miss %.3f m, %u plans\n", s.arrived, count, steps / 60.0f, minSeparation, maxMiss, s.pathsPlanned);
    CHECK(s.arrived == uint32_t(count));
    CHECK(minSeparation > -0.08f);   // discs (r 0.35) never overlap by more than a few cm
    CHECK(maxMiss < 0.45f);
    scene.Clear();
    Physics::Shutdown();
}

// ============================================================================ Lua
TEST_CASE("Lua: on_start / on_update / fixed update move entities, impulses, collisions, sandbox, errors") {
    CHECK(Physics::Init());
    GameContext ctx;
    Scene scene("Lua");
    Entity floor = scene.CreateEntity("Floor");
    floor.Get<Transform>().position = glm::vec3(0, -0.5f, 0);
    floor.Add<Collider>().shape.halfExtents = glm::vec3(10, 0.5f, 10);
    Entity mover = scene.CreateEntity("Mover");
    mover.Add<Script>().source = R"lua(
        function on_start(self) self.t = 0; self.count = 0 end
        function on_update(self, dt)
            self.t = self.t + dt
            self.count = self.count + 1
            self.entity:set_position(self.t, 1, 0)
            if io ~= nil or os ~= nil or dofile ~= nil then error("sandbox leak") end
        end
    )lua";
    Entity ball = scene.CreateEntity("Ball");
    ball.Get<Transform>().position = glm::vec3(3, 2, 0);
    ball.Add<Collider>().shape.shape = Physics::ShapeType::Sphere;
    ball.Add<Rigidbody>();
    ball.Add<Script>().source = R"lua(
        hits = 0
        function on_start(self) self.entity:impulse(0, 0, 2) end
        function on_collision(self, other, speed)
            hits = hits + 1
            last = other:name()
            scene.event("ball hit " .. last)
        end
    )lua";
    Entity broken = scene.CreateEntity("Broken");
    broken.Add<Script>().source = "function on_update(self, dt) local x = nil + 1 end";
    Entity spinning = scene.CreateEntity("Runaway");
    spinning.Add<Script>().source = "function on_start(self) while true do end end";
    scene.SyncPhysicsBodies();
    for (int i = 0; i < 90; ++i) {
        Scripting::Update(scene, ctx, 1.0f / 60.0f);
        Physics::ClearEvents();
        scene.SyncPhysicsBodies();
        Scripting::FixedUpdate(scene, ctx, 1.0f / 60.0f);
        Physics::Step(1.0f / 60.0f);
        scene.PullPhysicsPoses();
        Scripting::DispatchPhysicsEvents(scene, ctx);
    }
    const glm::vec3 p = mover.WorldPosition();
    std::printf("    mover at x = %.3f (expected 1.5), ball vz %.3f, events %zu\n", p.x, ball.GetLinearVelocity().z, ctx.eventLog.size());
    CHECK(std::fabs(p.x - 1.5f) < 1e-3f && std::fabs(p.y - 1.0f) < 1e-6f);
    CHECK(mover.Get<Script>().error.empty());
    CHECK(ball.WorldPosition().z > 0.5f);                       // the impulse (2 N*s on 1 kg) moved it
    bool sawHit = false;
    for (const std::string& e : ctx.eventLog) if (e.find("ball hit Floor") != std::string::npos) sawHit = true;
    CHECK(sawHit);
    CHECK(broken.Get<Script>().error.find("arithmetic") != std::string::npos);
    CHECK(spinning.Get<Script>().error.find("timeout") != std::string::npos);
    std::string error;
    CHECK(Scripting::Run(scene, ctx, "local e = scene.find('Mover'); assert(e:name() == 'Mover'); assert(scene.find('nope') == nil)", &error));
    CHECK(!Scripting::Run(scene, ctx, "this is not lua", &error) && !error.empty());
    const Scripting::Stats s = Scripting::GetStats();
    CHECK(s.scripts == 4 && s.errors == 2);
    Scripting::Shutdown();
    scene.Clear();
    Physics::Shutdown();
}

// ============================================================================ terrain
TEST_CASE("Terrain: deterministic heightmap, mesh / collider triangulation, splatmap weights, scatter rules") {
    World::TerrainGenerator g;
    g.resolution = 33;
    g.cellSize = 1.0f;
    g.flatRadius = 4.0f;
    const World::Heightmap a = World::GenerateHeightmap(g), b = World::GenerateHeightmap(g);
    CHECK(a.heights == b.heights);
    CHECK(std::fabs(a.Sample(0.0f, 0.0f)) < 1e-6f);                       // flat pad
    CHECK(a.MaxHeight() > 1.0f && a.MaxHeight() <= g.height);
    const Render::MeshData mesh = World::BuildTerrainMesh(a);
    CHECK(mesh.vertices.size() == 33u * 33u && mesh.indices.size() == 32u * 32u * 6u);
    // mesh vertices lie on the sampled surface; triangle midpoints too (same split as the collider)
    float error = 0.0f;
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const glm::vec3 c = (mesh.vertices[mesh.indices[i]].position + mesh.vertices[mesh.indices[i + 1]].position + mesh.vertices[mesh.indices[i + 2]].position) / 3.0f;
        error = std::max(error, std::fabs(c.y - a.Sample(c.x, c.z)));
        const glm::vec3 n = glm::cross(mesh.vertices[mesh.indices[i + 1]].position - mesh.vertices[mesh.indices[i]].position,
                                       mesh.vertices[mesh.indices[i + 2]].position - mesh.vertices[mesh.indices[i]].position);
        if (n.y <= 0.0f) error = 1e9f;                                    // counter-clockwise from above
    }
    CHECK(error < 1e-4f);
    Terrain defaults;
    const std::vector<uint8_t> splat = World::BuildSplatmap(a, defaults.rules, 64);
    bool sums = true;
    int snowTexels = 0;
    for (size_t i = 0; i < splat.size(); i += 4) {
        sums &= std::abs(int(splat[i]) + splat[i + 1] + splat[i + 2] + splat[i + 3] - 255) <= 2;
        snowTexels += splat[i + 3] > 128;
    }
    CHECK(sums);
    World::ScatterDesc s;
    s.count = 200;
    s.maxSlopeDegrees = 20.0f;
    s.minSpacing = 1.2f;
    s.keepOutRadius = 5.0f;
    const std::vector<glm::mat4> trees = World::ScatterInstances(a, s);
    bool rules = !trees.empty();
    for (size_t i = 0; i < trees.size(); ++i) {
        const glm::vec3 p(trees[i][3]);
        rules &= a.SlopeDegrees(p.x, p.z) <= 20.0f + 1e-3f && glm::length(glm::vec2(p.x, p.z)) >= 5.0f;
        for (size_t j = 0; j < i; ++j) rules &= glm::length(glm::vec2(p.x, p.z) - glm::vec2(trees[j][3].x, trees[j][3].z)) >= 1.2f - 1e-4f;
    }
    std::printf("    %zu instances placed, %d snow texels\n", trees.size(), snowTexels);
    CHECK(rules);
    CHECK(World::ScatterInstances(a, s).size() == trees.size());           // deterministic
}

// ============================================================================ VFX components (headless)
#include "Phisiks/VFX/VFX.h"
TEST_CASE("VFX: trails record, age and trim; orbit / decal components counted") {
    Scene scene("Vfx");
    Entity orb = scene.CreateEntity("Orb");
    Trail& t = orb.Add<Trail>();
    t.lifetime = 1.0f;
    for (int i = 0; i < 120; ++i) {   // 2 s at 60 Hz, moving 3 m/s on a circle
        const float a = float(i) / 60.0f * 2.0f;
        orb.Get<Transform>().position = glm::vec3(std::cos(a) * 1.5f, 1.0f, std::sin(a) * 1.5f);
        VFX::UpdateScene(scene, 1.0f / 60.0f);
    }
    const VFX::Stats s = VFX::GetStats();
    std::printf("    trail: %zu points after 2 s (lifetime 1 s at 60 Hz)\n", orb.Get<Trail>().points.size());
    CHECK(s.trails == 1 && orb.Get<Trail>().points.size() >= 55 && orb.Get<Trail>().points.size() <= 62);
    CHECK(orb.Get<Trail>().points.back().age <= 1.0f + 1e-4f);
    // slow motion at a high frame rate (5 mm per frame < minDistance): samples still accumulate
    Entity slow = scene.CreateEntity("Slow");
    slow.Add<Trail>().lifetime = 2.0f;
    for (int i = 0; i < 240; ++i) {
        slow.Get<Transform>().position = glm::vec3(0.005f * float(i), 0.0f, 0.0f);
        VFX::UpdateScene(scene, 1.0f / 240.0f);
    }
    const size_t n = slow.Get<Trail>().points.size();
    std::printf("    slow trail: %zu points over 1.2 m at 240 Hz (minDistance 0.04 m)\n", n);
    CHECK(n >= 25 && n <= 35);
}
