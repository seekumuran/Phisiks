// Asset pipeline: STL parsing, the project AssetDatabase (discovery, .meta identity, import,
// duplicates, move / rename, external moves, missing files, reimport, references, reopening) and
// asset references in scene files.
#include "TestFramework.h"

#include "Phisiks/Assets/AssetDatabase.h"
#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Scene/Scene.h"
#include "Phisiks/Scene/SceneSerializer.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace Phisiks;
namespace fs = std::filesystem;

namespace {
    // A fresh folder under the system temp directory, removed by the destructor.
    struct TempDir {
        fs::path path;
        TempDir() {
            path = fs::temp_directory_path() / ("phisiks_assets_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            fs::create_directories(path);
        }
        ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
    };
    void WriteText(const fs::path& p, const std::string& text) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << text; }
    const char* CUBE_OBJ =
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3\nf 1 3 4\n";
    std::vector<uint8_t> BinaryStl(int facets) {
        std::vector<uint8_t> b(84 + size_t(facets) * 50, 0);
        uint32_t n = uint32_t(facets);
        std::memcpy(b.data() + 80, &n, 4);
        for (int i = 0; i < facets; ++i) {
            float v[12] = { 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0 };
            v[3] = float(i);   // shift each facet
            v[6] = float(i) + 1.0f;
            v[9] = float(i);
            std::memcpy(b.data() + 84 + size_t(i) * 50, v, sizeof(v));
        }
        return b;
    }
}

TEST_CASE("STL: binary and ASCII parse into flat-shaded triangles; garbage is rejected") {
    Render::MeshData mesh;
    std::string error;
    CHECK(Assets::ParseStl(BinaryStl(3), mesh, &error));
    CHECK(mesh.indices.size() == 9 && mesh.vertices.size() == 9);
    CHECK(std::fabs(glm::length(mesh.vertices[0].normal) - 1.0f) < 1e-5f);
    const std::string ascii = "solid t\n facet normal 0 0 1\n  outer loop\n   vertex 0 0 0\n   vertex 1 0 0\n   vertex 0 1 0\n  endloop\n endfacet\nendsolid t\n";
    CHECK(Assets::ParseStl(std::vector<uint8_t>(ascii.begin(), ascii.end()), mesh, &error));
    CHECK(mesh.indices.size() == 3 && std::fabs(mesh.vertices[0].normal.z - 1.0f) < 1e-5f);
    const std::string junk = "not a mesh";
    CHECK(!Assets::ParseStl(std::vector<uint8_t>(junk.begin(), junk.end()), mesh, &error) && !error.empty());
    CHECK(Assets::IsMeshFile("parts/Bracket.STL") && Assets::IsMeshFile("a.obj") && !Assets::IsMeshFile("Cube"));
}

TEST_CASE("AssetDatabase: discovery writes .meta files; identities persist across reopening") {
    TempDir dir;
    const fs::path root = dir.path / "assets";
    WriteText(root / "meshes" / "plate.obj", CUBE_OBJ);
    WriteText(root / "scripts" / "spin.lua", "function on_update(dt) end");
    WriteText(root / "notes.txt", "ignored");
    uint64_t plate = 0;
    {
        AssetDatabase db;
        std::string error;
        CHECK(db.Open(root.string(), &error, false));
        const std::vector<AssetInfo> all = db.List();
        CHECK(all.size() == 2);
        AssetInfo info;
        CHECK(db.FindByPath("meshes/plate.obj", info) && info.type == AssetType::Mesh && info.guid != 0 && info.hash != 0);
        plate = info.guid;
        CHECK(fs::exists(root / "meshes" / "plate.obj.meta"));
        CHECK(db.Folders().size() == 2);
    }
    AssetDatabase again;   // "restart"
    CHECK(again.Open(root.string(), nullptr, false));
    AssetInfo info;
    CHECK(again.Find(plate, info) && info.path == "meshes/plate.obj");
}

TEST_CASE("AssetDatabase: import, duplicates, move / rename, references") {
    TempDir dir;
    const fs::path outside = dir.path / "downloads";
    WriteText(outside / "bracket.obj", CUBE_OBJ);
    {
        std::ofstream out(outside / "part.stl", std::ios::binary);
        const std::vector<uint8_t> b = BinaryStl(2);
        out.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    }
    AssetDatabase db;
    CHECK(db.Open((dir.path / "assets").string(), nullptr, false));
    AssetImportResult a = db.Import((outside / "bracket.obj").string(), "meshes");
    CHECK(a.ok && !a.duplicate && fs::exists(dir.path / "assets" / "meshes" / "bracket.obj"));
    AssetImportResult again = db.Import((outside / "bracket.obj").string(), "other");
    CHECK(again.ok && again.duplicate && again.guid == a.guid && !fs::exists(dir.path / "assets" / "other" / "bracket.obj"));
    AssetImportResult stl = db.Import((outside / "part.stl").string(), "meshes");
    CHECK(stl.ok && stl.guid != a.guid);
    CHECK(!db.Import((outside / "missing.obj").string()).ok);
    CHECK(!db.Import((outside / "bracket.obj").string(), "../escape").ok);
    // references: stable across a rename / move
    const std::string ref = db.Reference(a.guid);
    CHECK(ref.rfind("asset:", 0) == 0 && ref.find("bracket.obj") != std::string::npos);
    uint64_t parsed = 0;
    CHECK(AssetDatabase::ParseReference(ref, parsed) && parsed == a.guid);
    std::string error;
    CHECK(db.Move(a.guid, "parts/renamed_bracket.obj", &error));
    CHECK(fs::exists(dir.path / "assets" / "parts" / "renamed_bracket.obj") && fs::exists(dir.path / "assets" / "parts" / "renamed_bracket.obj.meta"));
    CHECK(fs::path(db.Resolve(ref)).filename() == "renamed_bracket.obj");   // the old reference still resolves
    CHECK(!db.Move(a.guid, "../outside.obj", &error));
    CHECK(!db.Move(a.guid, "parts/renamed_bracket.png", &error));   // type cannot change
    Render::MeshData mesh;
    CHECK(Assets::LoadMesh(db.AbsolutePath(stl.guid), mesh, &error) && mesh.indices.size() == 6);
}

TEST_CASE("AssetDatabase: external moves keep identity; missing files are reported; reimport bumps the revision") {
    TempDir dir;
    const fs::path root = dir.path / "assets";
    const fs::path source = dir.path / "src" / "panel.obj";
    WriteText(source, CUBE_OBJ);
    AssetDatabase db;
    CHECK(db.Open(root.string(), nullptr, false));
    const AssetImportResult r = db.Import(source.string(), "meshes");
    CHECK(r.ok);
    // moved outside PHISIKS together with its .meta
    fs::create_directories(root / "moved");
    fs::rename(root / "meshes" / "panel.obj", root / "moved" / "panel.obj");
    fs::rename(root / "meshes" / "panel.obj.meta", root / "moved" / "panel.obj.meta");
    AssetRefreshReport rep = db.Refresh();
    AssetInfo info;
    CHECK(db.Find(r.guid, info) && info.path == "moved/panel.obj" && !info.missing && rep.moved == 1);
    // moved without its .meta: matched by content hash
    fs::rename(root / "moved" / "panel.obj", root / "panel.obj");
    rep = db.Refresh();
    CHECK(db.Find(r.guid, info) && info.path == "panel.obj" && !info.missing);
    CHECK(!fs::exists(root / "moved" / "panel.obj.meta"));   // the orphan .meta went with the identity
    // deleted: missing, still known
    fs::remove(root / "panel.obj");
    rep = db.Refresh();
    CHECK(db.Find(r.guid, info) && info.missing && rep.missing == 1 && db.Resolve(db.Reference(r.guid)).empty());
    // reimport from the source restores it; a changed source increments the revision
    AssetImportResult re = db.Reimport(r.guid);
    CHECK(re.ok && db.Find(r.guid, info) && !info.missing && fs::exists(root / "panel.obj"));
    const int revision = info.revision;
    WriteText(source, std::string(CUBE_OBJ) + "f 2 3 4\n");
    re = db.Reimport(r.guid);
    CHECK(re.ok && db.Find(r.guid, info) && info.revision == revision + 1);
    // a copied file with a copied .meta gets a new identity
    fs::copy_file(root / "panel.obj", root / "copy.obj");
    fs::copy_file(root / "panel.obj.meta", root / "copy.obj.meta");
    rep = db.Refresh();
    AssetInfo copy;
    CHECK(rep.duplicatesRenumbered == 1 && db.FindByPath("copy.obj", copy) && copy.guid != r.guid && db.Find(r.guid, info) && info.path == "panel.obj");
}

TEST_CASE("AssetDatabase: asset references in scene files resolve after a restart") {
    TempDir dir;
    const fs::path root = dir.path / "project" / "assets";
    WriteText(dir.path / "in" / "gear.obj", CUBE_OBJ);
    std::string sceneFile = (dir.path / "project" / "scenes" / "main.scene.json").string();
    std::string ref;
    {
        AssetDatabase db;
        CHECK(db.Open(root.string()));   // active: Assets::Resolve understands asset references
        const AssetImportResult r = db.Import((dir.path / "in" / "gear.obj").string(), "meshes");
        ref = db.Reference(r.guid);
        Scene s("Main");
        s.CreateEntity("Gear").Add<MeshRenderer>(MeshRenderer{ ref, "" });
        s.CreateEntity("Builtin").Add<MeshRenderer>(MeshRenderer{ "Cube", "" });
        CHECK(SceneSerializer::Save(s, sceneFile));
        const std::vector<AssetReference> refs = AssetDatabase::CollectReferences(s);
        CHECK(refs.size() == 1 && refs[0].resolved && refs[0].asset == r.guid && refs[0].field == "mesh");
        CHECK(!Assets::Resolve(ref).empty());
        db.Close();
        CHECK(Assets::Resolve(ref).empty());   // no active database: references do not resolve
    }
    AssetDatabase db;   // restart
    CHECK(db.Open(root.string()));
    Scene s("Main");
    std::string error;
    CHECK(SceneSerializer::Load(s, sceneFile, &error));
    Entity gear = s.FindByName("Gear");
    CHECK(gear && gear.Get<MeshRenderer>().mesh == ref && !Assets::Resolve(ref).empty());
    Render::MeshData mesh;
    CHECK(Assets::LoadMesh(ref, mesh, &error) && mesh.indices.size() == 6);
    // a missing asset is reported by CollectReferences
    fs::remove(root / "meshes" / "gear.obj");
    db.Refresh();
    const std::vector<AssetReference> refs = AssetDatabase::CollectReferences(s);
    CHECK(refs.size() == 1 && !refs[0].resolved);
    db.Close();
}

TEST_CASE("AssetTypeOf: supported formats") {
    CHECK(AssetTypeOf("a/b.OBJ") == AssetType::Mesh && AssetTypeOf("x.stl") == AssetType::Mesh);
    CHECK(AssetTypeOf("m.glb") == AssetType::Model && AssetTypeOf("m.gltf") == AssetType::Model);
    CHECK(AssetTypeOf("t.jpeg") == AssetType::Texture && AssetTypeOf("t.tga") == AssetType::Texture && AssetTypeOf("t.bmp") == AssetType::Texture);
    CHECK(AssetTypeOf("s.wav") == AssetType::Audio && AssetTypeOf("main.scene.json") == AssetType::Scene);
    CHECK(AssetTypeOf("p.step") == AssetType::Cad && AssetTypeOf("p.IGS") == AssetType::Cad && AssetTypeOf("p.phcad") == AssetType::CadDocument);
    CHECK(AssetTypeOf("p.obj.meta") == AssetType::Unknown && AssetTypeOf("readme.txt") == AssetType::Unknown && AssetTypeOf("data.json") == AssetType::Unknown);
}
