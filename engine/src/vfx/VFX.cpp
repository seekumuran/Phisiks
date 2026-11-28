#include "Phisiks/VFX/VFX.h"
#include "Phisiks/Core/Time.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Scene/Scene.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Phisiks::VFX {
    namespace {
        Stats g_stats;

        float Noise1(float x) {
            // smooth value noise on integer lattice
            auto hash = [](int i) { uint32_t h = uint32_t(i) * 374761393u + 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return float(h & 0xFFFFFF) / float(0xFFFFFF) * 2.0f - 1.0f; };
            const int i = int(std::floor(x));
            const float f = x - float(i);
            const float u = f * f * (3.0f - 2.0f * f);
            return hash(i) + (hash(i + 1) - hash(i)) * u;
        }

        glm::vec3 Perpendicular(const glm::vec3& d) {
            const glm::vec3 axis = std::fabs(d.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            return glm::normalize(glm::cross(d, axis));
        }
    }

    void UpdateScene(Scene& scene, float dt) {
        Stats stats;
        std::vector<Render::RibbonPoint> ribbon;

        // ---- trails
        for (auto [handle, trail] : scene.Registry().view<Trail>().each()) {
            ++stats.trails;
            for (Trail::Point& p : trail.points) p.age += dt;
            while (!trail.points.empty() && trail.points.back().age > trail.lifetime) trail.points.pop_back();
            const Entity entity(&scene, handle);
            const glm::vec3 head = glm::vec3(entity.WorldMatrix() * glm::vec4(trail.offset, 1.0f));
            if (trail.emitting) {
                // points[0] is the live head (follows the entity every frame); points[1..] are committed
                // samples. A new sample is committed once the head is minDistance from the last one,
                // independent of the frame rate.
                if (trail.points.empty()) { trail.points.push_front({ head, 0.0f }); trail.points.push_front({ head, 0.0f }); }
                else {
                    trail.points.front() = { head, 0.0f };
                    if (trail.points.size() < 2 || glm::length(head - trail.points[1].position) >= trail.minDistance) trail.points.push_front({ head, 0.0f });
                }
                while (trail.points.size() > trail.maxPoints) trail.points.pop_back();
            }
            if (trail.points.size() < 2) continue;
            ribbon.clear();
            for (const Trail::Point& p : trail.points) {
                const float t = std::clamp(p.age / std::max(trail.lifetime, 1e-3f), 0.0f, 1.0f);
                ribbon.push_back({ p.position, trail.width + (trail.tailWidth - trail.width) * t, glm::mix(trail.headColor, trail.tailColor, t) });
            }
            stats.trailPoints += uint32_t(ribbon.size());
            Render::DrawRibbon(ribbon);
        }

        // ---- beams
        const float time = Time::TotalTime();
        for (auto [handle, beam] : scene.Registry().view<Beam>().each()) {
            if (!beam.enabled) continue;
            ++stats.beams;
            const Entity entity(&scene, handle);
            const glm::vec3 start = glm::vec3(entity.WorldMatrix() * glm::vec4(beam.offset, 1.0f));
            glm::vec3 end = beam.endPoint;
            if (!beam.target.empty()) if (Entity target = scene.FindByName(beam.target)) end = target.WorldPosition();
            if (beam.raycast) {
                const glm::vec3 d = end - start;
                const float length = glm::length(d);
                Physics::QueryOptions options;
                options.ignoreBody = entity.GetBody();
                if (length > 1e-4f) if (auto hit = Physics::Raycast(start, d / length, length, options)) end = hit->point;
            }
            beam.lastEnd = end;
            const glm::vec3 axis = end - start;
            const float length = glm::length(axis);
            if (length < 1e-4f) continue;
            const glm::vec3 dir = axis / length;
            const glm::vec3 side = Perpendicular(dir), up = glm::cross(dir, side);
            const int segments = std::clamp(beam.segments, 2, 256);
            ribbon.clear();
            for (int i = 0; i <= segments; ++i) {
                const float t = float(i) / float(segments);
                const float envelope = std::sin(t * 3.14159265f);   // pinned at both ends
                const float n1 = Noise1(t * 7.0f + time * beam.frequency + float(entt::to_integral(handle) & 1023u) * 3.1f);
                const float n2 = Noise1(t * 7.0f - time * beam.frequency * 0.8f + 11.0f + float(entt::to_integral(handle) & 1023u));
                const glm::vec3 p = start + axis * t + (side * n1 + up * n2) * beam.jitter * envelope;
                const float flicker = 0.85f + 0.15f * Noise1(time * 23.0f + t * 3.0f);
                ribbon.push_back({ p, beam.width * (0.6f + 0.4f * envelope + 0.3f), beam.color * flicker });
            }
            Render::DrawRibbon(ribbon);
            // bright core
            for (Render::RibbonPoint& p : ribbon) { p.width *= 0.3f; p.color = glm::vec4(glm::vec3(p.color) * 0.5f + glm::vec3(2.0f), p.color.a); }
            Render::DrawRibbon(ribbon);
        }

        // ---- orbit guides
        for (auto [handle, orbit] : scene.Registry().view<OrbitPath>().each()) {
            ++stats.orbits;
            const Entity entity(&scene, handle);
            const glm::mat4 world = glm::translate(glm::mat4(1.0f), entity.WorldPosition()) * glm::mat4_cast(entity.WorldRotation());
            const int segments = std::clamp(orbit.segments, 8, 512);
            ribbon.clear();
            for (int i = 0; i <= segments; ++i) {
                const float a = float(i) / float(segments) * 6.2831853f;
                const glm::vec3 p = glm::vec3(world * glm::vec4(std::cos(a) * orbit.radiusX, 0.0f, std::sin(a) * orbit.radiusZ, 1.0f));
                glm::vec4 color = orbit.color;
                if (orbit.dash > 0.0f && std::fmod(float(i) / float(segments), orbit.dash * 2.0f) > orbit.dash) color.a = 0.0f;
                ribbon.push_back({ p, orbit.width, color });
            }
            Render::DrawRibbon(ribbon);
        }

        // ---- decals
        for (auto [handle, decal] : scene.Registry().view<Decal>().each()) {
            if (!decal.visible) continue;
            ++stats.decals;
            const Entity entity(&scene, handle);
            Render::DecalDesc d;
            d.world = glm::translate(glm::mat4(1.0f), entity.WorldPosition()) * glm::mat4_cast(entity.WorldRotation()) * glm::scale(glm::mat4(1.0f), decal.size * entity.WorldScale());
            d.color = decal.color;
            d.pattern = decal.pattern;
            d.blend = decal.blend;
            d.angleFade = decal.angleFade;
            d.seed = decal.seed;
            Render::DrawDecal(d);
        }
        g_stats = stats;
    }

    Stats GetStats() { return g_stats; }
}
