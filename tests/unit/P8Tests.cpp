// P8: worker pool + CPU compute backend, FSR 1.0 reference, GL 4.1 shader rewrite, JSON scenes,
// replay scrubbing, CPU particles (emitter, two-way coupling) without a renderer.
#include "TestFramework.h"

#include "Phisiks/Compute/Compute.h"
#include "Phisiks/Framework/Replay.h"
#include "Phisiks/Physics/Advanced.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Render/Upscaler.h"
#include "Phisiks/Scene/Scene.h"
#include "Phisiks/Scene/SceneSerializer.h"
#include "compute/AdvancedWorld.h"
#include "compute/ThreadPool.h"
#include "render/ShaderPreprocessor.h"

#include <atomic>
#include <cmath>
#include <string>
#include <vector>

using namespace Phisiks;

namespace {
    std::vector<uint8_t> Image(int w, int h, auto f) {
        std::vector<uint8_t> img(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint8_t v = uint8_t(std::clamp(f(x, y), 0.0f, 255.0f));
                uint8_t* p = &img[(size_t(y) * w + x) * 4];
                p[0] = p[1] = p[2] = v;
                p[3] = 255;
            }
        return img;
    }
    int Channel(const std::vector<uint8_t>& img, int w, int x, int y) { return img[(size_t(y) * w + x) * 4]; }
}

// ============================================================================ worker pool / CPU backend
TEST_CASE("ThreadPool: every index exactly once; chunking independent of thread count") {
    std::vector<std::atomic<int>> hits(100003);
    for (auto& h : hits) h = 0;
    const size_t chunks = Compute::ThreadPool::Get().ParallelFor(hits.size(), 1000, [&](size_t, size_t b, size_t e) { for (size_t i = b; i < e; ++i) ++hits[i]; });
    bool once = true;
    for (auto& h : hits) once = once && h == 1;
    CHECK(once);
    CHECK(chunks == Compute::ThreadPool::ChunkCount(hits.size(), 1000));
    CHECK(Compute::ThreadPool::ChunkCount(0, 10) == 0);
}

TEST_CASE("Compute CPU backend: registered kernel, 2D dispatch, buffers and uniforms") {
    using namespace Compute;
    RegisterCpuKernel("unit_fill", 8, [](const CpuInvocation& inv) {
        const uint32_t w = inv.U("u_width");
        const uint32_t x = inv.globalId[0], y = inv.globalId[1];
        if (x >= w) return;
        inv.Buffer<float>(2)[y * w + x] = inv.F("u_scale") * float(x + 100 * y);
    });
    IComputeBackend* cpu = GetBackend(BackendType::CPU);
    CHECK(cpu != nullptr);
    if (!cpu) return;
    const uint32_t w = 37, h = 5;
    const BufferId b = cpu->CreateBuffer(w * h * 4);
    const KernelId k = cpu->CreateKernel("unit_fill", "");
    CHECK(k != 0);
    CHECK(cpu->CreateKernel("missing_kernel", "") == 0);
    cpu->SetBuffer(k, 2, b);
    cpu->SetUniform(k, "u_width", w);
    cpu->SetUniform(k, "u_scale", 0.5f);
    cpu->Dispatch(k, (w + 7) / 8, h);
    std::vector<float> out(w * h);
    cpu->Download(b, out.data(), out.size() * 4);
    bool exact = true;
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) exact = exact && out[y * w + x] == 0.5f * float(x + 100 * y);
    CHECK(exact);
    cpu->DestroyBuffer(b);
}

// ============================================================================ FSR 1.0 reference
TEST_CASE("FSR: constant images stay constant through EASU and RCAS") {
    const auto flat = Image(40, 30, [](int, int) { return 137.0f; });
    std::vector<uint8_t> up, sharp;
    Render::Fsr::Easu(flat, 40, 30, 60, 45, up);
    Render::Fsr::Rcas(up, 60, 45, 0.0f, sharp);
    bool same = true;
    for (size_t i = 0; i < sharp.size(); i += 4) same = same && up[i] == 137 && sharp[i] == 137;
    CHECK(same);
}

TEST_CASE("FSR: EASU follows a linear ramp and never rings at a step edge") {
    const auto ramp = Image(40, 8, [](int x, int) { return 40.0f + 4.0f * float(x); });
    std::vector<uint8_t> up;
    Render::Fsr::Easu(ramp, 40, 8, 60, 12, up);
    int worst = 0;
    for (int x = 3; x < 57; ++x) {   // interior (borders clamp)
        const float source = (float(x) + 0.5f) * 40.0f / 60.0f - 0.5f;
        worst = std::max(worst, std::abs(Channel(up, 60, x, 6) - int(std::lround(40.0f + 4.0f * source))));
    }
    CHECK(worst <= 2);
    const auto step = Image(32, 32, [](int x, int) { return x < 16 ? 30.0f : 220.0f; });
    std::vector<uint8_t> easu, sharp, bilinear;
    Render::Fsr::Easu(step, 32, 32, 48, 48, easu);
    Render::Fsr::Rcas(easu, 48, 48, 0.0f, sharp);
    Render::Fsr::Bilinear(step, 32, 32, 48, 48, bilinear);
    bool bounded = true, mildOvershoot = true;
    int transitionEasu = 0, transitionBilinear = 0;
    for (int x = 0; x < 48; ++x) {
        const int u = Channel(easu, 48, x, 24), e = Channel(sharp, 48, x, 24), b = Channel(bilinear, 48, x, 24);
        bounded = bounded && u >= 30 && u <= 220;                       // EASU: clamped to the quad (no ringing)
        mildOvershoot = mildOvershoot && e >= 30 - 48 && e <= 220 + 48;  // RCAS: lobe limited (< 25% of the step)
        transitionEasu += e > 40 && e < 210;
        transitionBilinear += b > 40 && b < 210;
    }
    CHECK(bounded);
    CHECK(mildOvershoot);
    CHECK(transitionEasu <= transitionBilinear);    // edge at least as sharp as bilinear
}

TEST_CASE("FSR: EASU + RCAS reconstruct a downsampled image better than bilinear") {
    const int W = 192, H = 144;
    const auto truth = Image(W, H, [](int x, int y) {   // rings: edges in every direction
        const float r = std::sqrt(float((x - 96) * (x - 96) + (y - 72) * (y - 72)));
        return std::fmod(r, 18.0f) < 9.0f ? 200.0f : 50.0f;
    });
    const int w = W * 2 / 3, h = H * 2 / 3;
    std::vector<uint8_t> low;
    Render::Fsr::Bilinear(truth, W, H, w, h, low);
    std::vector<uint8_t> easu, fsr, bilinear;
    Render::Fsr::Easu(low, w, h, W, H, easu);
    Render::Fsr::Rcas(easu, W, H, 0.25f, fsr);
    Render::Fsr::Bilinear(low, w, h, W, H, bilinear);
    const double pFsr = Render::Fsr::Psnr(fsr, truth), pBilinear = Render::Fsr::Psnr(bilinear, truth);
    CHECK(pFsr > pBilinear);
    CHECK(Render::Fsr::Psnr(truth, truth) == 99.0);
}

// ============================================================================ GL 4.1 shader rewrite
TEST_CASE("GL 4.1 tier: layout(binding) rewritten and recorded") {
    const std::string src = "#version 460 core\nlayout(std140, binding = 0) uniform FrameData { mat4 m; };\n"
                            "layout(binding = 7) uniform sampler2D u_font;\nlayout(binding = 12) uniform usamplerBuffer u_remap;\nvoid main() {}\n";
    Render::ShaderPreprocessor::ResourceBindings bindings;
    const std::string out = Render::ShaderPreprocessor::RewriteForGL41(src, bindings);
    CHECK(out.rfind("#version 410 core\n#define PHISIKS_GL41 1", 0) == 0);
    CHECK(out.find("binding") == std::string::npos);
    CHECK(out.find("layout(std140) uniform FrameData") != std::string::npos);
    CHECK(out.find("uniform sampler2D u_font;") != std::string::npos);
    CHECK(bindings.blocks.size() == 1 && bindings.blocks[0].first == "FrameData" && bindings.blocks[0].second == 0);
    CHECK(bindings.samplers.size() == 2 && bindings.samplers[0] == std::make_pair(std::string("u_font"), 7) && bindings.samplers[1].second == 12);
}

// ============================================================================ JSON scenes
TEST_CASE("SceneSerializer: JSON round trip keeps entities, hierarchy and every component") {
    Scene a("JsonA");
    Entity root = a.CreateEntity("Root");
    root.Get<Transform>().position = glm::vec3(1, 2, 3);
    root.Get<Transform>().rotation = glm::angleAxis(0.7f, glm::normalize(glm::vec3(1, 2, 0)));
    root.Add<MeshRenderer>(MeshRenderer{ "Sphere", "" });
    Entity child = a.CreateEntity("Child \"quoted\"");
    child.SetParent(root, false);
    child.Get<Transform>().scale = glm::vec3(0.5f, 2.0f, 1.0f);
    Collider c;
    c.shape.shape = Physics::ShapeType::Capsule;
    c.shape.radius = 0.3f;
    c.shape.halfHeight = 0.9f;
    c.shape.material.restitution = 0.4f;
    c.layer = 3;
    child.Add<Collider>(c);
    Rigidbody rb;
    rb.type = Physics::BodyType::Kinematic;
    rb.mass = 7.5f;
    child.Add<Rigidbody>(rb);
    Entity light = a.CreateEntity("Spot");
    light.Add<Light>().type = LightType::Spot;
    light.Get<Light>().outerConeAngle = 0.9f;
    Entity field = a.CreateEntity("Field");
    Physics::ForceField ff;
    ff.type = Physics::FieldType::Vortex;
    ff.strength = 3.5f;
    ff.affectsSoftBodies = false;
    field.Add<Physics::ForceField>(ff);
    Physics::Advanced::ParticleSystem ps;
    ps.emissionRate = 250.0f;
    ps.lifetime = 2.0f;
    ps.pushRigidBodies = false;
    field.Add<Physics::Advanced::ParticleSystem>(ps);
    Physics::Advanced::SoftBody sb;
    sb.kind = Physics::Advanced::SoftBodyKind::Rope;
    sb.resolution = glm::ivec3(31, 1, 1);
    a.CreateEntity("Rope").Add<Physics::Advanced::SoftBody>(sb);

    const std::string json = SceneSerializer::ToJson(a);
    Scene b("JsonA");
    std::string error;
    CHECK(SceneSerializer::FromJson(b, json, &error));
    CHECK(error.empty());
    CHECK(b.EntityCount() == a.EntityCount());
    CHECK(SceneSerializer::ToJson(b) == json);   // lossless (entity ids are not part of the file)
    Entity child2 = b.FindByName("Child \"quoted\"");
    CHECK(child2 && child2.GetParent() == b.FindByName("Root"));
    CHECK(child2 && child2.Has<Collider>() && child2.Get<Collider>().shape.shape == Physics::ShapeType::Capsule && child2.Get<Collider>().layer == 3);
    CHECK(child2 && std::fabs(child2.Get<Rigidbody>().mass - 7.5f) < 1e-6f && child2.Get<Rigidbody>().type == Physics::BodyType::Kinematic);
    CHECK(glm::length(b.FindByName("Root").Get<Transform>().position - glm::vec3(1, 2, 3)) < 1e-6f);
    const Entity f2 = b.FindByName("Field");
    CHECK(f2 && f2.Get<Physics::ForceField>().type == Physics::FieldType::Vortex && !f2.Get<Physics::ForceField>().affectsSoftBodies);
    CHECK(f2 && f2.Get<Physics::Advanced::ParticleSystem>().emissionRate == 250.0f && !f2.Get<Physics::Advanced::ParticleSystem>().pushRigidBodies);
    CHECK(b.FindByName("Rope").Get<Physics::Advanced::SoftBody>().resolution.x == 31);
    CHECK(!SceneSerializer::FromJson(b, "{ not json", &error) && !error.empty());
    CHECK(!SceneSerializer::FromJson(b, "{\"format\":\"other\"}", &error));
}

// ============================================================================ replay
TEST_CASE("Replay: restore any step exactly, resume truncates the timeline") {
    CHECK(Physics::Init());
    {
        Scene scene("Replay");
        Entity ground = scene.CreateEntity("Ground");
        ground.Get<Transform>().position = glm::vec3(0, -0.5f, 0);
        ground.Add<Collider>().shape.halfExtents = glm::vec3(20, 0.5f, 20);
        Entity box = scene.CreateEntity("Box");
        box.Get<Transform>().position = glm::vec3(0, 4, 0);
        box.Add<Collider>();
        box.Add<Rigidbody>();
        scene.SyncPhysicsBodies();
        Replay replay;
        replay.SetEnabled(true);
        replay.SetCapacity(100);
        std::vector<float> heights;
        for (int i = 0; i < 120; ++i) {
            Physics::Step(1.0f / 60.0f);
            scene.PullPhysicsPoses();
            replay.Record(scene, uint64_t(i + 1), (i + 1) / 60.0);
            heights.push_back(box.WorldPosition().y);
        }
        CHECK(replay.Size() == 100);                    // ring capacity
        CHECK(replay.At(0).step == 21);
        CHECK(replay.Restore(scene, 10));               // step 31
        CHECK(std::fabs(box.WorldPosition().y - heights[30]) < 1e-6f);
        CHECK(std::fabs(Physics::GetPosition(box.GetBody()).y - heights[30]) < 1e-6f);
        // continuing from the restored state follows the recorded trajectory (velocities restored too)
        for (int i = 0; i < 10; ++i) { Physics::Step(1.0f / 60.0f); scene.PullPhysicsPoses(); }
        CHECK(std::fabs(box.WorldPosition().y - heights[40]) < 0.01f);
        replay.Record(scene, 41, 41 / 60.0);
        CHECK(!replay.Scrubbed() && replay.Size() == 12);   // 0..10 kept + the new one
        CHECK(!replay.Restore(scene, 50));
    }
    Physics::Shutdown();
}

// ============================================================================ CPU particles without a renderer
TEST_CASE("CPU compute: emitter rate / lifetime and two-way coupling (headless)") {
    CHECK(Physics::Init());
    CHECK(Advanced::Init());   // no renderer: CPU worker-pool backend
    {
        Scene scene("Emitter");
        Entity emitter = scene.CreateEntity("Emitter");
        Physics::Advanced::ParticleSystem ps;
        ps.count = 1000;
        ps.emissionRate = 600.0f;
        ps.lifetime = 0.5f;
        ps.spawnSize = glm::vec3(0.3f, 0.0f, 0.3f);
        ps.emitDirection = glm::vec3(0, 1, 0);
        ps.emitSpeed = 6.0f;
        ps.emitSpread = 0.05f;
        ps.useGravity = false;
        ps.radius = 0.03f;
        ps.mass = 0.05f;
        emitter.Add<Physics::Advanced::ParticleSystem>(ps);
        Entity box = scene.CreateEntity("Target");
        box.Get<Transform>().position = glm::vec3(0, 1.5f, 0);
        box.Add<Collider>();
        Rigidbody rb;
        rb.useGravity = false;
        rb.mass = 1.0f;
        box.Add<Rigidbody>(rb);
        scene.SyncPhysicsBodies();
        for (int i = 0; i < 60; ++i) {
            scene.SyncPhysicsBodies();
            Physics::Step(1.0f / 60.0f);
            Advanced::Step(scene, 1.0f / 60.0f);
            scene.PullPhysicsPoses();
        }
        const Physics::Advanced::Stats s = Physics::Advanced::GetStats();
        CHECK(s.cpuBackend && !s.gpuAvailable);
        CHECK(s.particlesEmitted >= 595 && s.particlesEmitted <= 605);        // 600 / s for 1 s
        CHECK(s.particlesAlive >= 290 && s.particlesAlive <= 310);            // rate x lifetime
        CHECK(s.coupledImpulsesApplied > 0);
        CHECK(box.WorldPosition().y > 1.5f + 0.01f);                         // pushed up by the jet
        std::vector<glm::vec4> particles;
        CHECK(Physics::Advanced::ReadParticles(emitter, particles) && particles.size() == 1000);
    }
    Advanced::Shutdown();
    Physics::Shutdown();
}

TEST_CASE("Physics: SetBodyState restores pose and velocities") {
    CHECK(Physics::Init());
    Physics::BodyDesc d;
    d.colliders.push_back(Physics::ColliderDesc{});
    d.enableGravity = false;
    const Physics::BodyId b = Physics::CreateBody(d);
    Physics::SetBodyState(b, glm::vec3(1, 2, 3), glm::angleAxis(0.5f, glm::vec3(0, 1, 0)), glm::vec3(0, 0, 2), glm::vec3(0, 1, 0));
    CHECK(glm::length(Physics::GetPosition(b) - glm::vec3(1, 2, 3)) < 1e-5f);
    CHECK(glm::length(Physics::GetLinearVelocity(b) - glm::vec3(0, 0, 2)) < 1e-5f);
    Physics::Step(1.0f / 60.0f);
    CHECK(std::fabs(Physics::GetPosition(b).z - (3.0f + 2.0f / 60.0f)) < 1e-3f);
    CHECK(std::string(Physics::BackendName()).size() > 3);
    Physics::Shutdown();
}
