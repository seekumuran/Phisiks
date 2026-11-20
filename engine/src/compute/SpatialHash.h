#pragma once
// CPU reference of the GPU spatial hash grid (engine/shaders/spatial_hash.glsl uses the same cell
// hash). Used by unit tests and by the runtime check that compares GPU neighbour counts.

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace Phisiks::Compute::SpatialHash {
    // Multiplication by the inverse cell size and squared-distance tests are used on both CPU and
    // GPU: they are correctly rounded in GLSL (division and sqrt are not), so results match exactly.
    inline glm::ivec3 Cell(const glm::vec3& p, float cellSize) {
        const float inv = 1.0f / cellSize;
        return glm::ivec3(int(std::floor(p.x * inv)), int(std::floor(p.y * inv)), int(std::floor(p.z * inv)));
    }
    inline bool Within(const glm::vec3& a, const glm::vec3& b, float radius) {
        const glm::vec3 d = a - b;
        return d.x * d.x + d.y * d.y + d.z * d.z < radius * radius;
    }

    // Large-prime XOR hash of the integer cell, reduced to [0, tableSize).
    inline uint32_t Hash(const glm::ivec3& c, uint32_t tableSize) {
        const uint32_t h = (uint32_t(c.x) * 73856093u) ^ (uint32_t(c.y) * 19349663u) ^ (uint32_t(c.z) * 83492791u);
        return h % tableSize;
    }

    struct Grid {
        float cellSize = 1.0f;
        uint32_t tableSize = 1;
        std::vector<uint32_t> cellStart;    // tableSize + 1 (exclusive prefix sum of counts)
        std::vector<uint32_t> sorted;       // particle indices grouped by cell
    };

    // Counting sort into hash buckets (what the GPU does with count / scan / scatter passes).
    inline Grid Build(const std::vector<glm::vec3>& positions, float cellSize, uint32_t tableSize) {
        Grid g;
        g.cellSize = cellSize;
        g.tableSize = tableSize;
        std::vector<uint32_t> count(tableSize, 0);
        for (const glm::vec3& p : positions) ++count[Hash(Cell(p, cellSize), tableSize)];
        g.cellStart.assign(tableSize + 1, 0);
        for (uint32_t i = 0; i < tableSize; ++i) g.cellStart[i + 1] = g.cellStart[i] + count[i];
        std::vector<uint32_t> cursor(g.cellStart.begin(), g.cellStart.end() - 1);
        g.sorted.resize(positions.size());
        for (uint32_t i = 0; i < positions.size(); ++i) g.sorted[cursor[Hash(Cell(positions[i], cellSize), tableSize)]++] = i;
        return g;
    }

    // Number of other particles within `radius` of particle i (radius <= cellSize). Hash collisions
    // put unrelated particles in a bucket; the distance test rejects them, but a bucket shared by
    // two of the 27 neighbour cells must only be scanned once.
    inline uint32_t CountNeighbours(const Grid& g, const std::vector<glm::vec3>& positions, uint32_t i, float radius) {
        const glm::ivec3 c = Cell(positions[i], g.cellSize);
        uint32_t visited[27];
        int visitedCount = 0;
        uint32_t n = 0;
        for (int z = -1; z <= 1; ++z)
            for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x) {
                    const uint32_t h = Hash(c + glm::ivec3(x, y, z), g.tableSize);
                    bool seen = false;
                    for (int k = 0; k < visitedCount; ++k) seen |= visited[k] == h;
                    if (seen) continue;
                    visited[visitedCount++] = h;
                    for (uint32_t s = g.cellStart[h]; s < g.cellStart[h + 1]; ++s) {
                        const uint32_t j = g.sorted[s];
                        if (j != i && Within(positions[j], positions[i], radius)) ++n;
                    }
                }
        return n;
    }

    inline uint32_t CountNeighboursBruteForce(const std::vector<glm::vec3>& positions, uint32_t i, float radius) {
        uint32_t n = 0;
        for (uint32_t j = 0; j < positions.size(); ++j) if (j != i && Within(positions[j], positions[i], radius)) ++n;
        return n;
    }
}
