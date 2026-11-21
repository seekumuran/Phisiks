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
// (when a renderer is running). Loading destroys the scene's entities and rebuilds them (entity ids
// change); physics bodies, render objects and simulations are created by the scene systems as for
// any new entity. Unknown keys are ignored; missing keys keep the component defaults.

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
