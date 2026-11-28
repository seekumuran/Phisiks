// Heightmap terrain, splatmap, foliage scatter and their scene system.
#include "Phisiks/World/Terrain.h"
#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Scene/Scene.h"
#include "scene/SceneInternal.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <unordered_map>

namespace Phisiks::World {
    namespace {
        uint32_t Hash(uint32_t x) {
            x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
            return x;
        }
        float Lattice(int x, int z, uint32_t seed) {
            return float(Hash(uint32_t(x) * 73856093u ^ uint32_t(z) * 19349663u ^ seed * 83492791u) & 0xFFFFFF) / float(0xFFFFFF);
        }
        float ValueNoise(float x, float z, uint32_t seed) {
            const int ix = int(std::floor(x)), iz = int(std::floor(z));
            const float fx = x - float(ix), fz = z - float(iz);
            const float ux = fx * fx * (3.0f - 2.0f * fx), uz = fz * fz * (3.0f - 2.0f * fz);
            const float a = Lattice(ix, iz, seed), b = Lattice(ix + 1, iz, seed), c = Lattice(ix, iz + 1, seed), d = Lattice(ix + 1, iz + 1, seed);
            return (a + (b - a) * ux) + ((c + (d - c) * ux) - (a + (b - a) * ux)) * uz;
        }
        float Smooth(float e0, float e1, float x) {
            const float t = std::clamp((x - e0) / std::max(e1 - e0, 1e-6f), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }
        struct Rng {
            uint32_t state;
            explicit Rng(uint32_t seed) : state(Hash(seed + 0x9E3779B9u) | 1u) {}
            float Next() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return float(state & 0xFFFFFF) / float(0x1000000); }
            float Range(float a, float b) { return a + (b - a) * Next(); }
        };
    }

    // ====================================================================== heightmap
    float Heightmap::At(int x, int z) const {
        if (columns == 0 || rows == 0) return 0.0f;
        x = std::clamp(x, 0, int(columns) - 1);
        z = std::clamp(z, 0, int(rows) - 1);
        return heights[size_t(z) * columns + size_t(x)];
    }

    float Heightmap::Sample(float lx, float lz) const {
        if (columns < 2 || rows < 2) return 0.0f;
        const float gx = lx / cellSize + 0.5f * float(columns - 1), gz = lz / cellSize + 0.5f * float(rows - 1);
        const int x = std::clamp(int(std::floor(gx)), 0, int(columns) - 2), z = std::clamp(int(std::floor(gz)), 0, int(rows) - 2);
        const float fx = std::clamp(gx - float(x), 0.0f, 1.0f), fz = std::clamp(gz - float(z), 0.0f, 1.0f);
        const float h00 = At(x, z), h10 = At(x + 1, z), h01 = At(x, z + 1), h11 = At(x + 1, z + 1);
        // same split as the collider / mesh: triangle (00, 01, 10) and (10, 01, 11)
        if (fx + fz <= 1.0f) return h00 + fx * (h10 - h00) + fz * (h01 - h00);
        return h11 + (1.0f - fx) * (h01 - h11) + (1.0f - fz) * (h10 - h11);
    }

    glm::vec3 Heightmap::Normal(float lx, float lz) const {
        const float e = cellSize;
        const float dx = Sample(lx + e, lz) - Sample(lx - e, lz);
        const float dz = Sample(lx, lz + e) - Sample(lx, lz - e);
        return glm::normalize(glm::vec3(-dx, 2.0f * e, -dz));
    }

    float Heightmap::SlopeDegrees(float lx, float lz) const {
        return std::acos(std::clamp(Normal(lx, lz).y, -1.0f, 1.0f)) * 57.2957795f;
    }

    float Heightmap::MinHeight() const { return heights.empty() ? 0.0f : *std::min_element(heights.begin(), heights.end()); }
    float Heightmap::MaxHeight() const { return heights.empty() ? 0.0f : *std::max_element(heights.begin(), heights.end()); }

    Heightmap GenerateHeightmap(const TerrainGenerator& g) {
        Heightmap h;
        h.columns = h.rows = std::max(g.resolution, 2u);
        h.cellSize = std::max(g.cellSize, 1e-3f);
        h.heights.resize(size_t(h.columns) * h.rows);
        const float half = 0.5f * float(h.columns - 1) * h.cellSize;
        for (uint32_t z = 0; z < h.rows; ++z) {
            for (uint32_t x = 0; x < h.columns; ++x) {
                const float wx = float(x) * h.cellSize - half, wz = float(z) * h.cellSize - half;
                float value = 0.0f, amplitude = 1.0f, total = 0.0f, frequency = g.frequency;
                for (int o = 0; o < std::max(g.octaves, 1); ++o) {
                    const float n = ValueNoise(wx * frequency + 31.7f * o, wz * frequency - 17.3f * o, g.seed + uint32_t(o) * 1013u);
                    const float ridged = 1.0f - std::fabs(2.0f * n - 1.0f);
                    value += amplitude * (n + (ridged * ridged - n) * g.ridge);
                    total += amplitude;
                    amplitude *= g.persistence;
                    frequency *= 2.0f;
                }
                float height = g.height * std::clamp(value / std::max(total, 1e-6f), 0.0f, 1.0f);
                const float r = std::sqrt(wx * wx + wz * wz);
                if (g.flatRadius > 0.0f) height *= Smooth(g.flatRadius, g.flatRadius + std::max(g.flatBlend, 1e-3f), r);
                h.heights[size_t(z) * h.columns + x] = height;
            }
        }
        return h;
    }

    bool HeightmapFromImage(const std::string& path, float cellSize, float height, Heightmap& out) {
        Assets::Image image;
        if (!Assets::LoadImageFile(path, image) || image.width < 2 || image.height < 2) return false;
        out.columns = uint32_t(image.width);
        out.rows = uint32_t(image.height);
        out.cellSize = std::max(cellSize, 1e-3f);
        out.heights.resize(size_t(out.columns) * out.rows);
        for (uint32_t z = 0; z < out.rows; ++z)
            for (uint32_t x = 0; x < out.columns; ++x)
                out.heights[size_t(z) * out.columns + x] = float(image.rgba[(size_t(z) * out.columns + x) * 4]) / 255.0f * height;
        return true;
    }

    Render::MeshData BuildTerrainMesh(const Heightmap& h) {
        Render::MeshData mesh;
        if (h.columns < 2 || h.rows < 2) return mesh;
        const float hx = 0.5f * float(h.columns - 1) * h.cellSize, hz = 0.5f * float(h.rows - 1) * h.cellSize;
        mesh.vertices.reserve(size_t(h.columns) * h.rows);
        for (uint32_t z = 0; z < h.rows; ++z) {
            for (uint32_t x = 0; x < h.columns; ++x) {
                const float lx = float(x) * h.cellSize - hx, lz = float(z) * h.cellSize - hz;
                mesh.vertices.push_back({ glm::vec3(lx, h.At(int(x), int(z)), lz), h.Normal(lx, lz),
                                          glm::vec2(float(x) / float(h.columns - 1), float(z) / float(h.rows - 1)) });
            }
        }
        mesh.indices.reserve(size_t(h.columns - 1) * (h.rows - 1) * 6);
        for (uint32_t z = 0; z + 1 < h.rows; ++z) {
            for (uint32_t x = 0; x + 1 < h.columns; ++x) {
                const uint32_t i00 = z * h.columns + x, i10 = i00 + 1, i01 = i00 + h.columns, i11 = i01 + 1;
                mesh.indices.insert(mesh.indices.end(), { i00, i01, i10, i10, i01, i11 });
            }
        }
        return mesh;
    }

    std::vector<uint8_t> BuildSplatmap(const Heightmap& h, const SplatRule rules[4], int size, uint32_t seed) {
        size = std::clamp(size, 8, 2048);
        std::vector<uint8_t> rgba(size_t(size) * size * 4, 0);
        const glm::vec2 extent = h.Extent();
        for (int row = 0; row < size; ++row) {
            for (int col = 0; col < size; ++col) {
                const float lx = (float(col) + 0.5f) / float(size) * extent.x - 0.5f * extent.x;
                const float lz = (float(row) + 0.5f) / float(size) * extent.y - 0.5f * extent.y;
                const float height = h.Sample(lx, lz), slope = h.SlopeDegrees(lx, lz);
                float w[4];
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    const SplatRule& r = rules[k];
                    const float n = (ValueNoise(lx * 0.35f, lz * 0.35f, seed + uint32_t(k) * 77u) - 0.5f) * 2.0f * r.noise;
                    const float hb = std::max(r.blend, 1e-3f), sb = std::max(r.blend * 3.0f, 3.0f);
                    const float hv = height + n * hb * 2.0f, sv = slope + n * sb * 2.0f;
                    const float inHeight = Smooth(r.minHeight - hb, r.minHeight + hb, hv) * (1.0f - Smooth(r.maxHeight - hb, r.maxHeight + hb, hv));
                    const float inSlope = Smooth(r.minSlope - sb, r.minSlope + sb, sv) * (1.0f - Smooth(r.maxSlope - sb, r.maxSlope + sb, sv));
                    w[k] = inHeight * inSlope;
                    sum += w[k];
                }
                if (sum < 1e-4f) { w[0] = 1.0f; sum = 1.0f; }
                uint8_t* p = &rgba[(size_t(row) * size + col) * 4];
                int total = 0;
                for (int k = 0; k < 3; ++k) { p[k] = uint8_t(std::lround(w[k] / sum * 255.0f)); total += p[k]; }
                p[3] = uint8_t(std::clamp(255 - total, 0, 255));
            }
        }
        return rgba;
    }

    std::vector<glm::mat4> ScatterInstances(const Heightmap& h, const ScatterDesc& d) {
        std::vector<glm::mat4> out;
        if (h.columns < 2 || h.rows < 2 || d.count == 0) return out;
        Rng rng(d.seed);
        const glm::vec2 extent = h.Extent();
        const float spacing = std::max(d.minSpacing, 0.0f);
        const float bucket = std::max(spacing, 0.5f);
        std::unordered_map<int64_t, std::vector<glm::vec2>> grid;
        auto key = [&](float x, float z) { return (int64_t(std::floor(x / bucket)) << 32) ^ (int64_t(std::floor(z / bucket)) & 0xFFFFFFFF); };
        const uint32_t attempts = d.count * 30u;
        for (uint32_t a = 0; a < attempts && out.size() < d.count; ++a) {
            const float x = rng.Range(-0.5f, 0.5f) * extent.x * 0.98f, z = rng.Range(-0.5f, 0.5f) * extent.y * 0.98f;
            const float yaw = rng.Range(0.0f, 6.2831853f), scale = rng.Range(d.minScale, d.maxScale);
            if (d.keepOutRadius > 0.0f && x * x + z * z < d.keepOutRadius * d.keepOutRadius) continue;
            const float height = h.Sample(x, z);
            if (height < d.minHeight || height > d.maxHeight || h.SlopeDegrees(x, z) > d.maxSlopeDegrees) continue;
            if (spacing > 0.0f) {
                bool blocked = false;
                const int bx = int(std::floor(x / bucket)), bz = int(std::floor(z / bucket));
                for (int ox = -1; ox <= 1 && !blocked; ++ox)
                    for (int oz = -1; oz <= 1 && !blocked; ++oz) {
                        auto it = grid.find((int64_t(bx + ox) << 32) ^ (int64_t(bz + oz) & 0xFFFFFFFF));
                        if (it == grid.end()) continue;
                        for (const glm::vec2& p : it->second) if (glm::length(p - glm::vec2(x, z)) < spacing) { blocked = true; break; }
                    }
                if (blocked) continue;
                grid[key(x, z)].push_back(glm::vec2(x, z));
            }
            glm::quat rotation = glm::angleAxis(yaw, glm::vec3(0, 1, 0));
            if (d.alignToNormal) {
                const glm::vec3 n = h.Normal(x, z);
                const glm::vec3 axis = glm::cross(glm::vec3(0, 1, 0), n);
                const float s = glm::length(axis);
                if (s > 1e-5f) rotation = glm::angleAxis(std::asin(std::min(s, 1.0f)), axis / s) * rotation;
            }
            out.push_back(glm::translate(glm::mat4(1.0f), glm::vec3(x, height - d.sink * scale, z)) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), glm::vec3(scale)));
        }
        return out;
    }

    // ====================================================================== foliage meshes
    namespace {
        void AddCone(Render::MeshData& m, float y0, float y1, float r0, float r1, int sides) {
            const uint32_t base = uint32_t(m.vertices.size());
            const float slope = (r0 - r1) / std::max(y1 - y0, 1e-4f);
            for (int i = 0; i <= sides; ++i) {
                const float a = float(i) / float(sides) * 6.2831853f;
                const glm::vec3 dir(std::cos(a), 0.0f, std::sin(a));
                const glm::vec3 n = glm::normalize(glm::vec3(dir.x, slope, dir.z));
                m.vertices.push_back({ glm::vec3(dir.x * r0, y0, dir.z * r0), n, glm::vec2(float(i) / sides, 0.0f) });
                m.vertices.push_back({ glm::vec3(dir.x * r1, y1, dir.z * r1), n, glm::vec2(float(i) / sides, 1.0f) });
            }
            for (int i = 0; i < sides; ++i) {
                const uint32_t a = base + uint32_t(i) * 2, b = a + 1, c = a + 2, d = a + 3;
                m.indices.insert(m.indices.end(), { a, b, c, c, b, d });
            }
            // bottom cap (faces down)
            const uint32_t centre = uint32_t(m.vertices.size());
            m.vertices.push_back({ glm::vec3(0.0f, y0, 0.0f), glm::vec3(0, -1, 0), glm::vec2(0.5f) });
            for (int i = 0; i <= sides; ++i) {
                const float a = float(i) / float(sides) * 6.2831853f;
                m.vertices.push_back({ glm::vec3(std::cos(a) * r0, y0, std::sin(a) * r0), glm::vec3(0, -1, 0), glm::vec2(0.5f) });
            }
            for (int i = 0; i < sides; ++i) m.indices.insert(m.indices.end(), { centre, centre + 1 + uint32_t(i), centre + 2 + uint32_t(i) });
        }

        void AddBlob(Render::MeshData& m, glm::vec3 centre, glm::vec3 radius, float roughness, uint32_t seed, int rings = 10, int sectors = 14) {
            const uint32_t base = uint32_t(m.vertices.size());
            for (int r = 0; r <= rings; ++r) {
                const float v = float(r) / float(rings), phi = v * 3.14159265f;
                for (int s = 0; s <= sectors; ++s) {
                    const float u = float(s) / float(sectors), theta = u * 6.2831853f;
                    glm::vec3 dir(std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta));
                    const int sw = s == sectors ? 0 : s;   // seamless noise
                    const float n = 1.0f + roughness * (Lattice(sw, r, seed) - 0.5f) * 2.0f;
                    m.vertices.push_back({ centre + dir * radius * n, dir, glm::vec2(u, v) });
                }
            }
            for (int r = 0; r < rings; ++r)
                for (int s = 0; s < sectors; ++s) {
                    const uint32_t a = base + uint32_t(r * (sectors + 1) + s), b = a + uint32_t(sectors + 1);
                    m.indices.insert(m.indices.end(), { a, a + 1, b, b, a + 1, b + 1 });
                }
        }

        void SmoothNormals(Render::MeshData& m, uint32_t first) {
            for (size_t i = first; i < m.vertices.size(); ++i) m.vertices[i].normal = glm::vec3(0.0f);
            for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
                const uint32_t a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
                if (a < first) continue;
                const glm::vec3 n = glm::cross(m.vertices[b].position - m.vertices[a].position, m.vertices[c].position - m.vertices[a].position);
                m.vertices[a].normal += n; m.vertices[b].normal += n; m.vertices[c].normal += n;
            }
            for (size_t i = first; i < m.vertices.size(); ++i) {
                const float len = glm::length(m.vertices[i].normal);
                m.vertices[i].normal = len > 1e-12f ? m.vertices[i].normal / len : glm::vec3(0, 1, 0);
            }
        }
    }

    void RegisterFoliageMeshes() {
        if (!Render::HasMesh("Foliage/Pine")) {
            Render::MeshData m;
            AddCone(m, 0.0f, 0.55f, 0.09f, 0.07f, 8);                     // trunk
            AddCone(m, 0.45f, 1.45f, 0.75f, 0.05f, 12);
            AddCone(m, 1.05f, 2.05f, 0.58f, 0.04f, 12);
            AddCone(m, 1.65f, 2.6f, 0.40f, 0.0f, 12);
            Render::RegisterMesh("Foliage/Pine", m);
        }
        if (!Render::HasMesh("Foliage/Bush")) {
            Render::MeshData m;
            AddBlob(m, glm::vec3(0.0f, 0.35f, 0.0f), glm::vec3(0.55f, 0.4f, 0.55f), 0.25f, 11);
            AddBlob(m, glm::vec3(0.3f, 0.3f, 0.15f), glm::vec3(0.35f, 0.3f, 0.35f), 0.25f, 12);
            SmoothNormals(m, 0);
            Render::RegisterMesh("Foliage/Bush", m);
        }
        if (!Render::HasMesh("Foliage/Rock")) {
            Render::MeshData m;
            AddBlob(m, glm::vec3(0.0f, 0.15f, 0.0f), glm::vec3(0.6f, 0.38f, 0.5f), 0.35f, 21, 8, 10);
            SmoothNormals(m, 0);
            Render::RegisterMesh("Foliage/Rock", m);
        }
        if (!Render::HasMesh("Foliage/Grass")) {
            Render::MeshData m;
            for (int k = 0; k < 3; ++k) {   // three crossed double-sided quads
                const float a = float(k) / 3.0f * 3.14159265f;
                const glm::vec3 d(std::cos(a) * 0.3f, 0.0f, std::sin(a) * 0.3f), n(-std::sin(a), 0.4f, std::cos(a));
                for (int side = 0; side < 2; ++side) {
                    const uint32_t base = uint32_t(m.vertices.size());
                    const glm::vec3 nn = glm::normalize(side ? -n + glm::vec3(0, 0.8f, 0) : n);
                    m.vertices.push_back({ -d, nn, { 0, 0 } });
                    m.vertices.push_back({ d, nn, { 1, 0 } });
                    m.vertices.push_back({ d * 0.6f + glm::vec3(0, 0.45f, 0), nn, { 1, 1 } });
                    m.vertices.push_back({ -d * 0.6f + glm::vec3(0, 0.45f, 0), nn, { 0, 1 } });
                    if (side == 0) m.indices.insert(m.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
                    else m.indices.insert(m.indices.end(), { base, base + 2, base + 1, base, base + 3, base + 2 });
                }
            }
            Render::RegisterMesh("Foliage/Grass", m);
        }
        auto material = [](const char* name, glm::vec3 color, float roughness, Render::Pattern pattern = Render::Pattern::Noise, glm::vec3 accent = glm::vec3(0.0f)) {
            if (Render::HasMaterial(name)) return;
            Render::MaterialDesc d;
            d.baseColor = color;
            d.roughness = roughness;
            d.pattern = pattern;
            d.patternColor = accent == glm::vec3(0.0f) ? color * 0.6f : accent;
            d.patternScale = 3.0f;
            Render::RegisterMaterial(name, d);
        };
        material("Foliage/Pine", glm::vec3(0.06f, 0.16f, 0.07f), 0.85f);
        material("Foliage/Bush", glm::vec3(0.10f, 0.24f, 0.06f), 0.8f);
        material("Foliage/Rock", glm::vec3(0.30f, 0.29f, 0.28f), 0.75f, Render::Pattern::Noise, glm::vec3(0.18f, 0.17f, 0.16f));
        material("Foliage/Grass", glm::vec3(0.16f, 0.32f, 0.08f), 0.9f);
    }

    // ====================================================================== scene system
    namespace {
        struct TerrainRuntime { std::shared_ptr<const Heightmap> heightmap; };
        struct FoliageRuntime { std::vector<uint64_t> objects; };
        Stats g_stats;
        std::map<std::string, std::shared_ptr<const Heightmap>> g_built;   // key -> heightmap (mesh / texture / material registered once)

        void ReleaseFoliage(entt::registry& registry, entt::entity entity) {
            if (SceneInternal::IRenderBackend* render = SceneInternal::Render())
                for (uint64_t object : registry.get<FoliageRuntime>(entity).objects) render->DestroyMesh(object);
        }

        std::string KeyOf(const Terrain& t) {
            const TerrainGenerator& g = t.generator;
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "%u_%.4f_%.3f_%.4f_%d_%.3f_%.3f_%u_%.2f_%.2f_%d", g.resolution, double(g.cellSize), double(g.height), double(g.frequency),
                          g.octaves, double(g.persistence), double(g.ridge), g.seed, double(g.flatRadius), double(g.flatBlend), t.splatmapSize);
            return t.heightmapImage.empty() ? std::string(buffer) : t.heightmapImage + "_" + std::to_string(t.imageHeight) + "_" + std::to_string(g.cellSize);
        }
    }

    void UpdateScene(Scene& scene) {
        entt::registry& registry = scene.Registry();
        registry.on_destroy<FoliageRuntime>().connect<&ReleaseFoliage>();
        std::vector<entt::entity> pending;
        for (auto [handle, terrain] : registry.view<Terrain>().each()) if (!terrain.built) pending.push_back(handle);
        for (entt::entity handle : pending) {
            Terrain& terrain = registry.get<Terrain>(handle);
            terrain.built = true;
            const std::string key = KeyOf(terrain);
            std::shared_ptr<const Heightmap> heightmap;
            auto it = g_built.find(key);
            if (it != g_built.end()) heightmap = it->second;
            else {
                auto h = std::make_shared<Heightmap>();
                if (!terrain.heightmapImage.empty()) {
                    if (!HeightmapFromImage(terrain.heightmapImage, terrain.generator.cellSize, terrain.imageHeight, *h)) {
                        Log::Error() << "Terrain: heightmap image '" << terrain.heightmapImage << "' could not be loaded; using the generator";
                        *h = GenerateHeightmap(terrain.generator);
                    }
                }
                else *h = GenerateHeightmap(terrain.generator);
                heightmap = h;
                g_built[key] = heightmap;
            }
            const std::string name = "terrain:" + key;
            // layer colours / splat texture are per component: material per key + layer hash
            if (!Render::HasMesh(name)) Render::RegisterMesh(name, BuildTerrainMesh(*heightmap));
            const std::string splat = name + "#splat";
            Render::RegisterTexture(splat, terrain.splatmapSize, terrain.splatmapSize, BuildSplatmap(*heightmap, terrain.rules, terrain.splatmapSize));
            Render::MaterialDesc material;
            material.pattern = Render::Pattern::Splat;
            material.albedoTexture = splat;
            material.textureUsesMeshUV = true;
            material.patternScale = terrain.detailScale;
            material.roughness = 0.85f;
            for (int k = 0; k < 4; ++k) material.splat[k] = terrain.layers[k];
            const std::string materialName = name + "#" + std::to_string(entt::to_integral(handle));
            Render::RegisterMaterial(materialName, material);
            Entity entity(&scene, handle);
            MeshRenderer& renderer = entity.Add<MeshRenderer>();
            renderer.mesh = name;
            renderer.material = materialName;
            renderer.castShadows = true;
            if (terrain.collision) {
                Collider& collider = entity.Add<Collider>();
                collider.shape.shape = Physics::ShapeType::HeightField;
                collider.shape.heights = heightmap->heights;
                collider.shape.heightfieldColumns = heightmap->columns;
                collider.shape.heightfieldRows = heightmap->rows;
                collider.shape.heightfieldCellSize = heightmap->cellSize;
                collider.shape.material.staticFriction = 0.9f;
                collider.shape.material.dynamicFriction = 0.75f;
            }
            terrain.triangles = uint32_t((heightmap->columns - 1) * (heightmap->rows - 1) * 2);
            registry.emplace_or_replace<TerrainRuntime>(handle, TerrainRuntime{ heightmap });
            Log::Info() << "Terrain: '" << entity.GetName() << "' " << heightmap->columns << "x" << heightmap->rows << " samples, "
                        << heightmap->Extent().x << " m, heights " << heightmap->MinHeight() << " .. " << heightmap->MaxHeight() << " m";
        }

        // foliage
        // foliage: on the terrain entity itself or on a child of it
        std::vector<std::pair<entt::entity, entt::entity>> foliage;   // (scatter, terrain)
        for (auto [handle, scatter] : registry.view<FoliageScatter>().each()) {
            if (scatter.built) continue;
            entt::entity terrain = handle;
            if (!registry.all_of<TerrainRuntime>(terrain)) {
                const Entity parent = Entity(&scene, handle).GetParent();
                if (!parent || !registry.all_of<TerrainRuntime>(parent.Handle())) continue;
                terrain = parent.Handle();
            }
            foliage.emplace_back(handle, terrain);
        }
        for (const auto& [handle, terrainHandle] : foliage) {
            FoliageScatter& scatter = registry.get<FoliageScatter>(handle);
            scatter.built = true;
            RegisterFoliageMeshes();
            const std::vector<glm::mat4> instances = ScatterInstances(*registry.get<TerrainRuntime>(terrainHandle).heightmap, scatter.scatter);
            scatter.instances = uint32_t(instances.size());
            SceneInternal::IRenderBackend* render = SceneInternal::Render();
            if (!render) continue;
            MeshRenderer renderer;
            renderer.mesh = scatter.mesh;
            renderer.material = scatter.material.empty() ? scatter.mesh : scatter.material;
            renderer.castShadows = scatter.castShadows;
            const glm::mat4 world = Entity(&scene, terrainHandle).WorldMatrix();
            FoliageRuntime runtime;
            runtime.objects.reserve(instances.size());
            for (const glm::mat4& local : instances) runtime.objects.push_back(render->CreateMesh(renderer, world * local, uint32_t(entt::to_integral(terrainHandle))));
            registry.emplace_or_replace<FoliageRuntime>(handle, std::move(runtime));
        }

        Stats stats;
        for (auto [handle, terrain] : registry.view<Terrain>().each()) { ++stats.terrains; stats.terrainTriangles += terrain.triangles; }
        for (auto [handle, runtime] : registry.view<FoliageRuntime>().each()) { ++stats.foliageBatches; stats.foliageInstances += uint32_t(runtime.objects.size()); }
        g_stats = stats;
    }

    bool SampleTerrainHeight(Entity terrain, const glm::vec3& worldPosition, float& height) {
        if (!terrain.Valid()) return false;
        const TerrainRuntime* runtime = terrain.GetScene()->Registry().try_get<TerrainRuntime>(terrain.Handle());
        if (!runtime || !runtime->heightmap) return false;
        const glm::mat4 world = terrain.WorldMatrix();
        const glm::vec3 local = glm::vec3(glm::inverse(world) * glm::vec4(worldPosition, 1.0f));
        const glm::vec2 half = runtime->heightmap->Extent() * 0.5f;
        if (std::fabs(local.x) > half.x || std::fabs(local.z) > half.y) return false;
        height = glm::vec3(world * glm::vec4(local.x, runtime->heightmap->Sample(local.x, local.z), local.z, 1.0f)).y;
        return true;
    }

    Stats GetStats() { return g_stats; }
}
