#pragma once
// Phisiks::AssetDatabase: the assets of a project folder, with stable identities.
//
// Every supported file under the root gets a sidecar "<file>.meta" (JSON, "phisiks-asset" version
// 1): a 64-bit guid, the asset type, the content hash (FNV-1a 64), size and time stamp, the file it
// was imported from and a revision counter. Scenes reference assets as
//     "asset:<16 hex digit guid>/<file name>"     (Reference(guid))
// which Assets::Resolve maps to the asset's current file through the active database, so a
// reference survives moving or renaming the file (in the browser, or outside PHISIKS together with
// its .meta). A file moved outside PHISIKS without its .meta is matched to its old identity by
// content hash on the next Refresh. Plain relative paths ("meshes/arm.obj") keep working as before.
//
//   Phisiks::AssetDatabase db;
//   db.Open("MyProject/assets");                    // scans, writes missing .meta files
//   const auto r = db.Import("C:/Downloads/bracket.stl", "meshes");
//   entity.Add<MeshRenderer>().mesh = db.Reference(r.guid);   // "asset:3f2a.../bracket.stl"
//
// Import copies the file into the project (glTF: with its external buffers and images). Importing a
// file whose content is already in the project returns the existing asset (duplicate = true).
// Reimport copies the source again when it changed (revision + 1) and asks the renderer to reload.
// Thread-safe: Resolve may be called from the asset worker thread.
//
// Formats: see AssetType. STEP / IGES are CAD files: they are listed (AssetType::Cad) and opened by
// the CAD workspace's geometry kernel, not loaded as render meshes.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Phisiks {
    class Scene;

    enum class AssetType { Mesh, Model, Texture, Audio, Scene, Script, Cad, CadDocument, Unknown };
    const char* ToString(AssetType type);
    // By extension: .obj .stl -> Mesh; .gltf .glb -> Model; .png .jpg .jpeg .tga .bmp -> Texture;
    // .wav -> Audio; .scene.json -> Scene; .lua -> Script; .step .stp .iges .igs -> Cad;
    // .phcad -> CadDocument (PHISIKS parametric part); anything else -> Unknown (not listed).
    AssetType AssetTypeOf(const std::string& path);

    struct AssetInfo {
        uint64_t guid = 0;
        AssetType type = AssetType::Unknown;
        std::string path;           // relative to the root, '/' separators
        std::string name;           // file name
        uint64_t size = 0;          // bytes
        uint64_t hash = 0;          // FNV-1a 64 of the content
        std::string source;         // file it was imported from ("" = created in place)
        std::string imported;       // UTC time of the first import / discovery (ISO 8601)
        int revision = 1;           // + 1 whenever the content changed
        bool missing = false;       // known (a .meta exists) but the file is gone
        std::string error;          // last problem (unreadable file, broken .meta)
    };

    struct AssetImportResult {
        bool ok = false;
        uint64_t guid = 0;
        bool duplicate = false;     // the content was already in the project: the existing asset
        std::vector<std::string> copied;   // project-relative files written (glTF: + buffers / images)
        std::string error;
    };

    struct AssetRefreshReport {
        size_t assets = 0, added = 0, moved = 0, changed = 0, missing = 0, duplicatesRenumbered = 0;
        double milliseconds = 0.0;
    };

    // A scene's reference to an asset (component field holding a path or an asset reference).
    struct AssetReference {
        uint64_t entity = 0;        // EntityGuid
        std::string entityName;
        std::string component;      // SceneSerializer component key ("meshRenderer", ...) or "material:<name>"
        std::string field;          // "mesh", "clip", "albedoTexture", ...
        std::string value;          // as stored
        uint64_t asset = 0;         // guid when `value` is an asset reference
        bool resolved = false;      // Assets::Resolve finds the file
    };

    class AssetDatabase {
    public:
        AssetDatabase();
        ~AssetDatabase();
        AssetDatabase(const AssetDatabase&) = delete;
        AssetDatabase& operator=(const AssetDatabase&) = delete;

        // Opens (creating the folder if needed) and scans. Becomes the active database
        // (Assets::SetReferenceResolver + Assets::AddSearchPath(root)) unless `activate` is false.
        bool Open(const std::string& root, std::string* error = nullptr, bool activate = true);
        void Close();
        bool IsOpen() const;
        const std::string& Root() const;                           // absolute, '/' separators
        static AssetDatabase* Active();

        // Rescans the folder: new files get a .meta, moved files keep their guid, changed content
        // increments the revision (the renderer reloads it), files that are gone are marked missing.
        AssetRefreshReport Refresh();

        std::vector<AssetInfo> List(const std::string& folder = std::string(), bool recursive = true) const;
        std::vector<std::string> Folders() const;                 // relative folder paths (sorted)
        bool Find(uint64_t guid, AssetInfo& out) const;
        bool FindByPath(const std::string& relativePath, AssetInfo& out) const;
        std::string AbsolutePath(uint64_t guid) const;            // "" if unknown or missing

        AssetImportResult Import(const std::string& sourceFile, const std::string& folder = std::string());
        AssetImportResult Reimport(uint64_t guid);
        bool Move(uint64_t guid, const std::string& newRelativePath, std::string* error = nullptr);   // also renames
        bool CreateFolder(const std::string& relativePath, std::string* error = nullptr);

        // "asset:<guid>/<file name>"; "" if the guid is unknown.
        std::string Reference(uint64_t guid) const;
        static bool ParseReference(const std::string& reference, uint64_t& guid);
        std::string Resolve(const std::string& reference) const; // absolute path or ""

        // Every asset path / reference held by the scene's components and the materials they use.
        static std::vector<AssetReference> CollectReferences(Scene& scene);

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}
