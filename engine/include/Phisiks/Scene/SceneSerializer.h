#pragma once
// Human-readable JSON scene files.
//
//   { "format": "phisiks-scene", "version": 1, "name": "...",
//     "materials": { "<name>": { baseColor, roughness, metallic, emissive, pattern, ... } },
//     "entities": [ { "name", "parent" (index or -1), "transform", "meshRenderer", "light", "camera",
//                     "collider", "rigidbody", "audioSource", "forceField", "particleSystem",
//                     "softBody" } ] }
//
// Saving writes every entity with its built-in components and the procedural materials they use
// (when a renderer is running); Save writes "<path>.tmp" and renames it over the target, so a failed
// save keeps the previous file. Load parses and validates the whole file first: a damaged file or an
// unsupported "version" fails with an error and leaves the scene untouched. Only a valid file
// destroys the scene's entities and rebuilds them (entity ids change); physics bodies, render
// objects and simulations are created by the scene systems as for any new entity. Unknown keys are
// ignored; missing keys keep the component defaults. Not stored: Joint, Script, NavAgent, Terrain,
// Decal, Trail, SkinnedModel, CameraPath (re-create them in code after loading).

#include <string>

namespace Phisiks {
    class Scene;

    namespace SceneSerializer {
        std::string ToJson(Scene& scene, bool pretty = true);
        bool FromJson(Scene& scene, const std::string& json, std::string* error = nullptr);
        bool Save(Scene& scene, const std::string& path);
        bool Load(Scene& scene, const std::string& path, std::string* error = nullptr);
    }
}
