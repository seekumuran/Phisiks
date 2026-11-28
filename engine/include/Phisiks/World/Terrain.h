#pragma once
// Phisiks::World: heightmap terrain (procedural fBm or a greyscale image), 4-layer splatmap
// texturing, static height-field collision on both physics backends, and GPU-instanced foliage /
// prop scatter (each scattered mesh is one instanced draw command, culled on the GPU on the 4.6 tier).
//
//   Entity ground = scene.CreateEntity("Terrain");
//   Terrain& t = ground.Add<Terrain>();             // the scene system builds mesh, material, collider
//   ground.Add<FoliageScatter>().mesh = "Foliage/Pine";

#include "Phisiks/Render/Renderer.h"

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Phisiks {
    class Scene;
    class Entity;
}

namespace Phisiks::World {
    // Heights on a grid of columns (x) x rows (z), `cellSize` apart, centred on the origin.
    struct Heightmap {
        uint32_t columns = 0, rows = 0;
        float cellSize = 1.0f;
        std::vector<float> heights;            // heights[z * columns + x], metres

        float At(int x, int z) const;          // clamped to the grid
        float Sample(float localX, float localZ) const;   // bilinear over the same triangles as the collider
        glm::vec3 Normal(float localX, float localZ) const;
        float SlopeDegrees(float localX, float localZ) const;
        glm::vec2 Extent() const { return glm::vec2(float(columns > 0 ? columns - 1 : 0), float(rows > 0 ? rows - 1 : 0)) * cellSize; }
        float MinHeight() const;
        float MaxHeight() const;
    };

    struct TerrainGenerator {
        uint32_t resolution = 129;             // samples per side
        float cellSize = 0.5f;                 // metres
        float height = 6.0f;                   // metres (peak amplitude)
        float frequency = 0.035f;              // cycles per metre of the first octave
        int octaves = 5;
        float persistence = 0.5f;
        float ridge = 0.35f;                   // 0 = rolling fBm hills, 1 = sharp ridges
        uint32_t seed = 1;
        float flatRadius = 6.0f;               // central pad flattened to 0 (spawn area), metres
        float flatBlend = 4.0f;
    };
    Heightmap GenerateHeightmap(const TerrainGenerator& generator);
    // Greyscale (red channel) image -> heights in [0, height].
    bool HeightmapFromImage(const std::string& assetPath, float cellSize, float height, Heightmap& out);

    // Render mesh: one vertex per sample (UV 0..1 over the terrain), smooth normals, triangles split
    // exactly like the physics height field.
    Render::MeshData BuildTerrainMesh(const Heightmap& heightmap);

    // Layer selection for the splatmap: weight rises inside [minHeight, maxHeight] and
    // [minSlope, maxSlope] (degrees), with `blend` metres / degrees of soft edge.
    struct SplatRule {
        float minHeight = -1e9f, maxHeight = 1e9f;
        float minSlope = 0.0f, maxSlope = 90.0f;
        float blend = 1.0f;
        float noise = 0.15f;                   // value-noise breakup of the borders
    };
    // RGBA8 splatmap (rows top-first = +Z first... row 0 is z = -extent/2), weights sum to 255.
    std::vector<uint8_t> BuildSplatmap(const Heightmap& heightmap, const SplatRule rules[4], int size, uint32_t seed = 7);

    struct ScatterDesc {
        uint32_t count = 400;
        uint32_t seed = 3;
        float minHeight = -1e9f, maxHeight = 1e9f;
        float maxSlopeDegrees = 30.0f;
        float minScale = 0.7f, maxScale = 1.3f;
        float minSpacing = 0.8f;               // metres between instances (rejection sampling)
        float keepOutRadius = 0.0f;            // around the terrain centre (e.g. the flat pad)
        float sink = 0.05f;                    // metres pushed into the ground
        bool alignToNormal = false;            // rocks: yes, trees: no
    };
    // Instance transforms in terrain-local space (deterministic for a seed).
    std::vector<glm::mat4> ScatterInstances(const Heightmap& heightmap, const ScatterDesc& desc);

    // Procedural foliage / prop meshes: "Foliage/Pine", "Foliage/Bush", "Foliage/Rock", "Foliage/Grass".
    void RegisterFoliageMeshes();

    // Terrain height at a world position (x, z) under the entity's Terrain, false if outside / not built.
    bool SampleTerrainHeight(Entity terrain, const glm::vec3& worldPosition, float& height);

    void UpdateScene(Scene& scene);            // builds new Terrain / FoliageScatter components
    struct Stats { uint32_t terrains = 0, terrainTriangles = 0, foliageInstances = 0, foliageBatches = 0; };
    Stats GetStats();
}

namespace Phisiks {
    struct Terrain {
        World::TerrainGenerator generator;
        std::string heightmapImage;            // optional: overrides the generator (greyscale)
        float imageHeight = 8.0f;
        Render::SplatLayer layers[4] = {
            { { 0.20f, 0.32f, 0.10f }, 0.85f, 0.30f },   // R grass (lowlands)
            { { 0.36f, 0.30f, 0.22f }, 0.90f, 0.25f },   // G dirt (mid slopes)
            { { 0.38f, 0.38f, 0.40f }, 0.70f, 0.35f },   // B rock (steep)
            { { 0.85f, 0.88f, 0.92f }, 0.50f, 0.10f },   // A snow (peaks)
        };
        World::SplatRule rules[4] = {
            { -1e9f, 3.0f, 0.0f, 22.0f, 1.5f, 0.15f },
            { 0.5f, 4.5f, 12.0f, 34.0f, 2.0f, 0.2f },
            { -1e9f, 1e9f, 30.0f, 90.0f, 6.0f, 0.1f },
            { 4.2f, 1e9f, 0.0f, 40.0f, 0.8f, 0.2f },
        };
        int splatmapSize = 256;
        float detailScale = 0.6f;              // layer noise repeats per metre
        bool collision = true;
        // runtime
        bool built = false;
        uint32_t triangles = 0;
    };

    // Scatters instances of `mesh` over the Terrain on the same entity or on its parent (one
    // scatter per entity: put more on child entities of the terrain).
    struct FoliageScatter {
        std::string mesh = "Foliage/Pine";
        std::string material;                  // empty = a built-in foliage material for the mesh
        World::ScatterDesc scatter;
        bool castShadows = true;
        // runtime
        bool built = false;
        uint32_t instances = 0;
    };
}
