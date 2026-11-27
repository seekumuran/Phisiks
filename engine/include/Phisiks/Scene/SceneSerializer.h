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
// ignored; missing keys keep the component defaults. Not stored: Joint, Script, NavigationSurface,
// NavAgent, Terrain, FoliageScatter, Decal, Trail, Beam, OrbitPath, SkinnedModel, CameraPath, Health,
// Interactable (re-create them in code after loading). UnsavedComponents() lists the ones a scene
// holds, so a caller can warn before a save drops them; Save itself never refuses.

#include <cstddef>
#include <string>
#include <vector>

namespace Phisiks {
    class Scene;

    namespace SceneSerializer {
        // A component type present in the scene that scene files do not store.
        struct UnsavedComponent {
            std::string type;      // component type name, e.g. "Joint"
            size_t count = 0;      // number of entities that carry it
        };
        // Every such type in the scene, in a fixed order; empty when a save / load round trip keeps
        // all of the scene's component data.
        std::vector<UnsavedComponent> UnsavedComponents(Scene& scene);
        std::string ToJson(Scene& scene, bool pretty = true);
        bool FromJson(Scene& scene, const std::string& json, std::string* error = nullptr);
        bool Save(Scene& scene, const std::string& path);
        bool Load(Scene& scene, const std::string& path, std::string* error = nullptr);
    }
}
