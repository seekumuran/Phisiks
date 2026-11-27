#pragma once
// Human-readable JSON scene files, and the same JSON for single entities and components.
//
//   { "format": "phisiks-scene", "version": 2, "name": "...",
//     "materials": { "<name>": { baseColor, roughness, metallic, emissive, pattern, ... } },
//     "entities": [ { "id" (16 hex digits), "name", "parent" (index or -1), "transform",
//                     "<component key>": { ... }, ... } ] }
//
// Component keys (ComponentKeys()): meshRenderer, light, camera, collider, rigidbody, joint,
// audioSource, forceField, particleSystem, softBody, script, navigationSurface, navAgent, terrain,
// foliageScatter, decal, trail, beam, orbitPath, skinnedModel, cameraPath, health, interactable.
// Only authored fields are stored; runtime state (live bodies, Lua environments, trail points,
// agent paths) is rebuilt by the scene systems.
//
// Versions. Version 2 (PHISIKS 1.1) adds the entity "id" (EntityGuid) and every component from
// "joint" on. Version 1 files (PHISIKS 1.0) load unchanged: their entities get new ids, and the
// components they did not have keep their defaults. Files are always written as version 2; a 1.0
// build reports "unsupported version 2" for them.
//
// Safety. Saving writes "<path>.tmp" and renames it over the target, so a failed save keeps the
// previous file. Loading parses and validates the whole file (structure, versions, parent indices,
// duplicate ids, and every component value on a scratch scene) before the open scene is touched: a
// damaged file fails with an error and changes nothing. Unknown keys are ignored; missing keys keep
// the component defaults.
//
// Not stored: C++ callbacks (Health::onDeath, Interactable::onInteract). UnsavedComponents() lists
// the ones a scene holds, so a caller can warn before a save drops them; Save itself never refuses.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Phisiks {
    class Scene;
    class Entity;

    namespace SceneSerializer {
        inline constexpr int FORMAT_VERSION = 2;   // written by ToJson; FromJson reads 1..FORMAT_VERSION

        // ---- scene files
        std::string ToJson(Scene& scene, bool pretty = true);
        bool FromJson(Scene& scene, const std::string& json, std::string* error = nullptr);
        bool Save(Scene& scene, const std::string& path);
        bool Load(Scene& scene, const std::string& path, std::string* error = nullptr);
        // Brings `scene` back to a ToJson snapshot in place: entities whose id is in the snapshot keep
        // their handles and get the saved name, transform, components and parent; entities created
        // since are destroyed; ones destroyed since are re-created with their ids. Bodies are rebuilt
        // at the restored poses (velocities reset). Used by play mode (Stop restores the edit state).
        bool RestoreSnapshot(Scene& scene, const std::string& snapshot, std::string* error = nullptr);

        // ---- entities (one element of "entities", without "parent"; plus "children" and the
        // materials used when requested)
        std::string EntityToJson(Entity entity, bool withChildren = true, bool pretty = false);
        // Creates the entity tree of `json` under `parent` (invalid = root). keepGuids: the stored ids
        // are reused (an id already in use gets a new one, with a warning); false: every entity gets a
        // new id (duplicate / paste). Returns the root, or an invalid entity (scene unchanged) on error.
        Entity EntityFromJson(Scene& scene, const std::string& json, Entity parent, bool keepGuids, std::string* error = nullptr);
        // Name, transform and components of `entity` become those of `json` (components absent from
        // it are removed). Parent, children and id are not changed. Validated first: on error nothing changes.
        bool ApplyEntityJson(Entity entity, const std::string& json, std::string* error = nullptr);

        // ---- components by key
        const std::vector<std::string>& ComponentKeys();
        bool HasComponent(Entity entity, const std::string& key);
        std::string ComponentToJson(Entity entity, const std::string& key);   // "" if absent or unknown
        // Adds the component (from its defaults) or patches it: fields in `json` replace the current
        // values, the others stay. Validated first: on error nothing changes.
        bool SetComponentJson(Entity entity, const std::string& key, const std::string& json, std::string* error = nullptr);
        bool RemoveComponent(Entity entity, const std::string& key);

        // ---- named procedural materials (Render::RegisterMaterial)
        std::string MaterialToJson(const std::string& name);                  // "" if unknown
        bool SetMaterialJson(const std::string& name, const std::string& json, std::string* error = nullptr);   // patch or create

        // What a save would not keep: the C++ callbacks above.
        struct UnsavedComponent {
            std::string type;      // e.g. "Health.onDeath (C++ callback)"
            size_t count = 0;      // number of entities
        };
        std::vector<UnsavedComponent> UnsavedComponents(Scene& scene);
    }
}
