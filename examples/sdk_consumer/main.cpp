// A PHISIKS application built against the installed SDK (find_package(PHISIKS), Phisiks::Editor).
//
//   sdk_consumer --headless      deterministic simulation without a window: a ball dropped onto a
//                                floor, a scene file round trip, an asset database; prints the
//                                results and exits 0 when every check holds (used by ctest).
//   sdk_consumer                 the same scene in the PHISIKS editor (window, UI).
#include "EditorApp.h"                       // PhisiksEditor (include/PhisiksEditor)

#include "Phisiks/Assets/AssetDatabase.h"
#include "Phisiks/Cad/Document.h"            // PhisiksCad (shared library)
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Runtime/Application.h"
#include "Phisiks/Runtime/Game.h"
#include "Phisiks/Scene/Scene.h"
#include "Phisiks/Scene/SceneSerializer.h"
#include "Phisiks/Version.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

namespace {
    class DropScene : public Phisiks::Scene {
    public:
        DropScene() : Scene("Drop") {}
        void OnEnter(Phisiks::GameContext&) override {
            Phisiks::Entity floor = CreateEntity("Floor");
            floor.Get<Phisiks::Transform>().position = { 0.0f, -0.5f, 0.0f };
            floor.Get<Phisiks::Transform>().scale = { 10.0f, 1.0f, 10.0f };
            floor.Add<Phisiks::MeshRenderer>(Phisiks::MeshRenderer{ "Cube", "" });
            floor.Add<Phisiks::Collider>().shape.halfExtents = glm::vec3(0.5f);
            Phisiks::Entity ball = CreateEntity("Ball");
            ball.Get<Phisiks::Transform>().position = { 0.0f, 5.0f, 0.0f };
            ball.Add<Phisiks::MeshRenderer>(Phisiks::MeshRenderer{ "Sphere", "" });
            Phisiks::Collider c;
            c.shape.shape = Phisiks::Physics::ShapeType::Sphere;
            c.shape.radius = 0.5f;
            ball.Add<Phisiks::Collider>(c);
            ball.Add<Phisiks::Rigidbody>().mass = 2.0f;
            Phisiks::Entity sun = CreateEntity("Sun");
            sun.Add<Phisiks::Light>().type = Phisiks::LightType::Directional;
        }
    };

    // Headless: the game observes the scene at shutdown.
    class HeadlessGame : public Phisiks::IGame {
    public:
        float ballHeight = -1.0f;
        bool roundTrip = false;
        size_t entities = 0;
        void RegisterScenes(Phisiks::SceneManager& scenes) override { scenes.Register("Drop", [] { return std::make_unique<DropScene>(); }); }
        void OnUpdate(Phisiks::GameContext& ctx, float) override {
            Phisiks::Scene* scene = ctx.CurrentScene();
            if (!scene) return;
            if (Phisiks::Entity ball = scene->FindByName("Ball")) ballHeight = ball.WorldPosition().y;
            entities = scene->EntityCount();
            const std::string json = Phisiks::SceneSerializer::ToJson(*scene);
            Phisiks::Scene copy(scene->Name());   // same name: the JSON must match exactly
            roundTrip = Phisiks::SceneSerializer::FromJson(copy, json) && copy.EntityCount() == scene->EntityCount() && Phisiks::SceneSerializer::ToJson(copy) == json;
        }
    };

    int Headless() {
        std::printf("PHISIKS SDK consumer: PHISIKS %s (version macro %d)\n", PHISIKS_VERSION_STRING, PHISIKS_VERSION);
        HeadlessGame game;
        Phisiks::AppOptions options;
        options.headless = true;
        options.deterministic = true;
        options.maxFrames = 180;      // 3 s at 60 Hz
        options.initialScene = "Drop";
        options.runDirectory = (std::filesystem::temp_directory_path() / "phisiks_sdk_consumer").string();
        Phisiks::Application app;
        if (!app.Init(options, game)) { std::printf("PHISIKS SDK consumer: FAILED (Application::Init)\n"); return 1; }
        app.Run();
        // asset database: a project folder with one generated mesh
        const std::filesystem::path project = std::filesystem::temp_directory_path() / "phisiks_sdk_consumer_assets";
        std::error_code ec;
        std::filesystem::remove_all(project, ec);
        std::filesystem::create_directories(project / "meshes", ec);
        { std::FILE* f = std::fopen((project / "meshes" / "tri.obj").string().c_str(), "wb"); if (f) { std::fputs("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", f); std::fclose(f); } }
        Phisiks::AssetDatabase db;
        const bool dbOk = db.Open(project.string(), nullptr, false) && db.List().size() == 1 && db.List()[0].type == Phisiks::AssetType::Mesh;
        // CAD: a parametric plate with a hole, rebuilt after a parameter change
        Phisiks::Cad::Document part;
        part.SetParameter("w", "30");
        Phisiks::Cad::Feature plate;
        plate.type = Phisiks::Cad::FeatureType::Box;
        plate.values = { { "x", "w" }, { "y", "20" }, { "z", "5" } };
        part.AddFeature(plate);
        Phisiks::Cad::Sketch hole;
        hole.AddCircleAt(10, 10, 3);
        Phisiks::Cad::Feature holeSketch;
        holeSketch.type = Phisiks::Cad::FeatureType::Sketch;
        holeSketch.sketch = hole;
        const Phisiks::Cad::Id hs = part.AddFeature(holeSketch);
        Phisiks::Cad::Feature cut;
        cut.type = Phisiks::Cad::FeatureType::Extrude;
        cut.profileSketch = hs;
        cut.op = Phisiks::Cad::FeatureOp::Cut;
        cut.values["distance"] = "5";
        part.AddFeature(cut);
        part.SetParameter("w", "40");
        const Phisiks::Cad::RegenerateResult regen = part.Regenerate();
        const double volume = Phisiks::Cad::Kernel::Measure(part.Result()).volume;
        const bool kernel = Phisiks::Cad::Kernel::Available();
        const bool cadOk = kernel ? regen.ok && std::fabs(volume - (40.0 * 20.0 * 5.0 - 3.14159265358979 * 9.0 * 5.0)) < 0.01 : !regen.ok;
        std::printf("PHISIKS SDK consumer: CAD kernel %s, plate volume %.3f mm^3 (%s)\n", Phisiks::Cad::Kernel::Name().c_str(), volume, cadOk ? "ok" : "FAILED");
        const bool fell = game.ballHeight > 0.3f && game.ballHeight < 0.7f;   // resting on the floor (radius 0.5)
        std::printf("PHISIKS SDK consumer: ball at %.3f m after 3 s (%s), %zu entities, scene JSON round trip %s, asset database %s\n",
                    double(game.ballHeight), fell ? "resting on the floor" : "NOT on the floor", game.entities, game.roundTrip ? "ok" : "FAILED", dbOk ? "ok" : "FAILED");
        const bool ok = fell && game.roundTrip && dbOk && cadOk && game.entities >= 3;   // floor, ball, sun (+ the engine's camera)
        std::printf("PHISIKS SDK consumer: %s\n", ok ? "OK" : "FAILED");
        return ok ? 0 : 1;
    }
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--headless") == 0) return Headless();
    Editor::Launch launch;
    launch.app.windowTitle = "SDK consumer - PHISIKS";
    launch.app.initialScene = "Drop";
    if (const int code = Editor::ParseCommandLine(argc, argv, "sdk_consumer", launch)) return code == -1 ? 0 : code;
    Editor::AppConfig config;
    config.title = "SDK CONSUMER";
    Editor::Configure(config);
    Editor::Host host;
    host.registerScenes = [](Phisiks::SceneManager& scenes) {
        Editor::RegisterEmptyScene(scenes);
        scenes.Register("Drop", [] { return std::make_unique<DropScene>(); });
    };
    return Editor::Run(launch, host);
}
