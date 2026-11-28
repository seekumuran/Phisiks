// Navigation grid from static physics geometry, A* + string pulling, crowd steering.
#include "Phisiks/AI/Navigation.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Core/Time.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Scene/Scene.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <functional>
#include <queue>

namespace Phisiks::AI {
    namespace {
        bool StaticBody(Physics::BodyId id) {
            const Physics::BodyType t = Physics::GetBodyType(id);
            return t == Physics::BodyType::Static || t == Physics::BodyType::Kinematic;
        }
    }

    // ====================================================================== grid
    bool NavGrid::Build(const NavGridDesc& desc) {
        m_desc = desc;
        m_desc.cellSize = std::max(desc.cellSize, 0.02f);
        const glm::vec3 size = desc.boundsMax - desc.boundsMin;
        m_width = std::max(int(std::ceil(size.x / m_desc.cellSize)), 1);
        m_depth = std::max(int(std::ceil(size.z / m_desc.cellSize)), 1);
        const size_t cells = size_t(m_width) * m_depth;
        m_walkable.assign(cells, 0);
        m_height.assign(cells, 0.0f);
        m_region.assign(cells, -1);
        m_walkableCount = m_regionCount = 0;
        if (!Physics::IsInitialized() || size.y <= 0.0f) { m_width = m_depth = 0; return false; }

        const float minNormalY = std::cos(desc.maxSlopeDegrees * 0.0174532925f);
        Physics::QueryOptions options;
        options.layers = desc.layers;
        const float headRoom = std::max(desc.agentHeight - desc.maxStepHeight, 0.1f);
        for (int z = 0; z < m_depth; ++z) {
            for (int x = 0; x < m_width; ++x) {
                const float cx = desc.boundsMin.x + (float(x) + 0.5f) * m_desc.cellSize;
                const float cz = desc.boundsMin.z + (float(z) + 0.5f) * m_desc.cellSize;
                const glm::vec3 origin(cx, desc.boundsMax.y, cz);
                const std::vector<Physics::RaycastHit> hits = Physics::RaycastAll(origin, glm::vec3(0, -1, 0), size.y, options);
                const Physics::RaycastHit* floor = nullptr;
                for (const Physics::RaycastHit& h : hits) if (StaticBody(h.body)) { floor = &h; break; }
                if (!floor || floor->normal.y < minNormalY) continue;
                const size_t i = size_t(z) * m_width + x;
                m_height[i] = floor->point.y;
                // head room above the step height
                const glm::vec3 centre(cx, floor->point.y + desc.maxStepHeight + 0.5f * headRoom, cz);
                const glm::vec3 half(0.45f * m_desc.cellSize, 0.5f * headRoom - 0.01f, 0.45f * m_desc.cellSize);
                bool blocked = false;
                for (Physics::BodyId b : Physics::OverlapBox(centre, half, glm::quat(1, 0, 0, 0), options)) if (StaticBody(b)) { blocked = true; break; }
                m_walkable[i] = blocked ? 0 : 1;
            }
        }
        // Obstacles for the erosion: unwalkable cells and ledge cells (a 4-neighbour that is not
        // reachable by a step: wall feet, wall tops, drops). Cells closer than the agent radius to an
        // obstacle are removed, so agents keep their radius away from walls and narrow ledges vanish.
        std::vector<uint8_t> obstacle(cells, 0);
        for (int z = 0; z < m_depth; ++z) {
            for (int x = 0; x < m_width; ++x) {
                const size_t i = size_t(z) * m_width + x;
                if (!m_walkable[i]) { obstacle[i] = 1; continue; }
                const int nx[4] = { x + 1, x - 1, x, x }, nz[4] = { z, z, z + 1, z - 1 };
                for (int k = 0; k < 4; ++k) {
                    if (nx[k] < 0 || nz[k] < 0 || nx[k] >= m_width || nz[k] >= m_depth) continue;
                    const size_t n = size_t(nz[k]) * m_width + nx[k];
                    if (m_walkable[n] && std::fabs(m_height[n] - m_height[i]) > desc.maxStepHeight) { obstacle[i] = 1; break; }
                }
            }
        }
        for (size_t i = 0; i < cells; ++i) if (obstacle[i]) m_walkable[i] = 0;
        const int r = int(std::ceil(desc.agentRadius / m_desc.cellSize));
        if (r > 0) {
            std::vector<uint8_t> eroded = m_walkable;
            const float limit = desc.agentRadius / m_desc.cellSize + 0.5f;
            for (int z = 0; z < m_depth; ++z) {
                for (int x = 0; x < m_width; ++x) {
                    if (!obstacle[size_t(z) * m_width + x]) continue;
                    for (int dz = -r; dz <= r; ++dz)
                        for (int dx = -r; dx <= r; ++dx) {
                            const int nx = x + dx, nz = z + dz;
                            if (nx < 0 || nz < 0 || nx >= m_width || nz >= m_depth) continue;
                            if (std::sqrt(float(dx * dx + dz * dz)) <= limit) eroded[size_t(nz) * m_width + nx] = 0;
                        }
                }
            }
            m_walkable.swap(eroded);
        }
        for (uint8_t w : m_walkable) m_walkableCount += w;
        // connected regions
        std::vector<int> stack;
        for (int z = 0; z < m_depth; ++z) {
            for (int x = 0; x < m_width; ++x) {
                const size_t i = size_t(z) * m_width + x;
                if (!m_walkable[i] || m_region[i] >= 0) continue;
                const int region = int(m_regionCount++);
                stack.assign(1, int(i));
                m_region[i] = region;
                while (!stack.empty()) {
                    const int c = stack.back();
                    stack.pop_back();
                    const int cx = c % m_width, cz = c / m_width;
                    const int nx[4] = { cx + 1, cx - 1, cx, cx }, nz[4] = { cz, cz, cz + 1, cz - 1 };
                    for (int k = 0; k < 4; ++k) {
                        if (!IsWalkable(nx[k], nz[k]) || !Connected(cx, cz, nx[k], nz[k])) continue;
                        const size_t n = size_t(nz[k]) * m_width + nx[k];
                        if (m_region[n] >= 0) continue;
                        m_region[n] = region;
                        stack.push_back(int(n));
                    }
                }
            }
        }
        return true;
    }

    bool NavGrid::IsWalkable(int x, int z) const {
        return x >= 0 && z >= 0 && x < m_width && z < m_depth && m_walkable[size_t(z) * m_width + x] != 0;
    }
    float NavGrid::FloorHeight(int x, int z) const {
        x = std::clamp(x, 0, std::max(m_width - 1, 0));
        z = std::clamp(z, 0, std::max(m_depth - 1, 0));
        return m_height.empty() ? 0.0f : m_height[size_t(z) * m_width + x];
    }
    int NavGrid::Region(int x, int z) const { return IsWalkable(x, z) ? m_region[size_t(z) * m_width + x] : -1; }
    bool NavGrid::Connected(int x0, int z0, int x1, int z1) const {
        return std::fabs(FloorHeight(x0, z0) - FloorHeight(x1, z1)) <= m_desc.maxStepHeight;
    }

    bool NavGrid::WorldToCell(const glm::vec3& p, int& x, int& z) const {
        if (!Valid()) return false;
        x = int(std::floor((p.x - m_desc.boundsMin.x) / m_desc.cellSize));
        z = int(std::floor((p.z - m_desc.boundsMin.z) / m_desc.cellSize));
        return x >= 0 && z >= 0 && x < m_width && z < m_depth;
    }

    glm::vec3 NavGrid::CellCenter(int x, int z) const {
        return glm::vec3(m_desc.boundsMin.x + (float(x) + 0.5f) * m_desc.cellSize, FloorHeight(x, z), m_desc.boundsMin.z + (float(z) + 0.5f) * m_desc.cellSize);
    }

    bool NavGrid::NearestWalkable(const glm::vec3& p, int& x, int& z, int searchRadius) const {
        if (!Valid()) return false;
        int cx = int(std::floor((p.x - m_desc.boundsMin.x) / m_desc.cellSize)), cz = int(std::floor((p.z - m_desc.boundsMin.z) / m_desc.cellSize));
        cx = std::clamp(cx, 0, m_width - 1);
        cz = std::clamp(cz, 0, m_depth - 1);
        float best = FLT_MAX;
        bool found = false;
        for (int r = 0; r <= searchRadius && !found; ++r) {   // rings: nearest ring with a walkable cell
            for (int dz = -r; dz <= r; ++dz)
                for (int dx = -r; dx <= r; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dz)) != r || !IsWalkable(cx + dx, cz + dz)) continue;
                    const glm::vec3 c = CellCenter(cx + dx, cz + dz);
                    const float d = (c.x - p.x) * (c.x - p.x) + (c.z - p.z) * (c.z - p.z) + 0.25f * (c.y - p.y) * (c.y - p.y);
                    if (d < best) { best = d; x = cx + dx; z = cz + dz; found = true; }
                }
        }
        return found;
    }

    float NavGrid::FloorAt(const glm::vec3& p) const {
        int x, z;
        if (NearestWalkable(p, x, z, 2)) return FloorHeight(x, z);
        return WorldToCell(p, x, z) ? FloorHeight(x, z) : p.y;
    }

    bool NavGrid::LineOfSight(const glm::vec3& a, const glm::vec3& b) const {
        int px, pz;
        if (!WorldToCell(a, px, pz) || !IsWalkable(px, pz)) return false;
        const glm::vec3 d = b - a;
        const float length = std::sqrt(d.x * d.x + d.z * d.z);
        const int steps = std::max(int(std::ceil(length / (m_desc.cellSize * 0.25f))), 1);
        for (int i = 1; i <= steps; ++i) {
            const glm::vec3 p = a + d * (float(i) / float(steps));
            int x, z;
            if (!WorldToCell(p, x, z) || !IsWalkable(x, z)) return false;
            if (x != px || z != pz) {
                if (!Connected(px, pz, x, z)) return false;
                if (x != px && z != pz && (!IsWalkable(px, z) || !IsWalkable(x, pz))) return false;   // no squeezing past corners
                px = x; pz = z;
            }
        }
        return true;
    }

    PathResult NavGrid::FindPath(const glm::vec3& start, const glm::vec3& goal, bool smooth) const {
        PathResult result;
        int sx, sz, gx, gz;
        if (!NearestWalkable(start, sx, sz) || !NearestWalkable(goal, gx, gz)) return result;
        if (Region(sx, sz) != Region(gx, gz)) return result;   // different islands: no path
        const int startCell = sz * m_width + sx, targetCell = gz * m_width + gx;
        const size_t cells = size_t(m_width) * m_depth;
        std::vector<float> cost(cells, FLT_MAX);
        std::vector<int> parent(cells, -1);
        std::vector<uint8_t> closed(cells, 0);
        using Entry = std::pair<float, int>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
        auto heuristic = [&](int c) {
            const float dx = float(std::abs(c % m_width - gx)), dz = float(std::abs(c / m_width - gz));
            return (std::max(dx, dz) + 0.41421356f * std::min(dx, dz)) * m_desc.cellSize;   // octile
        };
        cost[startCell] = 0.0f;
        open.push({ heuristic(startCell), startCell });
        while (!open.empty()) {
            const int c = open.top().second;
            open.pop();
            if (closed[c]) continue;
            closed[c] = 1;
            ++result.expanded;
            if (c == targetCell) break;
            const int cx = c % m_width, cz = c / m_width;
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dz) continue;
                    const int nx = cx + dx, nz = cz + dz;
                    if (!IsWalkable(nx, nz) || !Connected(cx, cz, nx, nz)) continue;
                    if (dx && dz && (!IsWalkable(cx + dx, cz) || !IsWalkable(cx, cz + dz) || !Connected(cx, cz, cx + dx, cz) || !Connected(cx, cz, cx, cz + dz))) continue;
                    const int n = nz * m_width + nx;
                    if (closed[n]) continue;
                    const float step = (dx && dz ? 1.41421356f : 1.0f) * m_desc.cellSize;
                    const float g = cost[c] + step;
                    if (g < cost[n]) {
                        cost[n] = g;
                        parent[n] = c;
                        open.push({ g + heuristic(n), n });
                    }
                }
            }
        }
        if (!closed[targetCell]) return result;
        std::vector<glm::vec3> raw;
        for (int c = targetCell; c >= 0; c = parent[c]) raw.push_back(CellCenter(c % m_width, c / m_width));
        std::reverse(raw.begin(), raw.end());
        result.rawPoints = uint32_t(raw.size());
        // exact end points (projected onto the floor) when they are on walkable cells
        int tx, tz;
        if (WorldToCell(start, tx, tz) && IsWalkable(tx, tz) && tx == sx && tz == sz) raw.front() = glm::vec3(start.x, FloorHeight(tx, tz), start.z);
        if (WorldToCell(goal, tx, tz) && IsWalkable(tx, tz) && tx == gx && tz == gz) raw.back() = glm::vec3(goal.x, FloorHeight(tx, tz), goal.z);
        if (smooth && raw.size() > 2) {
            std::vector<glm::vec3> pulled{ raw.front() };
            size_t i = 0;
            while (i < raw.size() - 1) {
                size_t j = raw.size() - 1;
                while (j > i + 1 && !LineOfSight(raw[i], raw[j])) --j;
                pulled.push_back(raw[j]);
                i = j;
            }
            raw.swap(pulled);
        }
        result.points = std::move(raw);
        for (size_t k = 1; k < result.points.size(); ++k) result.length += glm::length(result.points[k] - result.points[k - 1]);
        result.found = true;
        return result;
    }

    // ====================================================================== crowd
    namespace {
        Stats g_stats;
    }

    void SetTarget(NavAgent& agent, const glm::vec3& target) {
        agent.target = target;
        agent.hasTarget = true;
        agent.arrived = false;
        agent.pathFailed = false;
        agent.path.clear();
    }

    NavGrid* SceneGrid(Scene& scene) {
        for (auto [handle, surface] : scene.Registry().view<NavigationSurface>().each()) return surface.grid ? surface.grid.get() : nullptr;
        return nullptr;
    }

    void UpdateScene(Scene& scene, float dt) {
        Stats stats;
        NavGrid* grid = nullptr;
        for (auto [handle, surface] : scene.Registry().view<NavigationSurface>().each()) {
            if (!surface.grid || surface.rebuild) {
                const double start = Time::NowSeconds();
                surface.grid = std::make_shared<NavGrid>();
                surface.grid->Build(surface.desc);
                surface.buildMs = float((Time::NowSeconds() - start) * 1000.0);
                surface.rebuild = false;
                Log::Info() << "Navigation: grid " << surface.grid->Width() << "x" << surface.grid->Depth() << " (" << surface.grid->WalkableCells() << " walkable cells, "
                            << surface.grid->Regions() << " regions) built in " << surface.buildMs << " ms";
            }
            grid = surface.grid.get();
            stats.buildMs = surface.buildMs;
            stats.walkableCells = grid->WalkableCells();
            break;
        }
        stats.pathsPlanned = g_stats.pathsPlanned;
        stats.pathFailures = g_stats.pathFailures;
        if (!grid || !grid->Valid() || dt <= 0.0f) { g_stats = stats; return; }

        struct Agent { entt::entity handle; NavAgent* agent; glm::vec3 position; };
        std::vector<Agent> agents;
        for (auto [handle, agent] : scene.Registry().view<NavAgent>().each()) agents.push_back({ handle, &agent, Entity(&scene, handle).WorldPosition() });
        stats.agents = uint32_t(agents.size());
        std::vector<glm::vec3> velocities(agents.size());
        for (size_t i = 0; i < agents.size(); ++i) {
            NavAgent& a = *agents[i].agent;
            const glm::vec3 p = agents[i].position;
            glm::vec3 desired(0.0f);
            if (a.hasTarget && !a.arrived) {
                a.repathTimer += dt;
                if (a.path.empty() || glm::length(a.target - a.pathTarget) > 0.5f || a.repathTimer > a.repathInterval) {
                    const PathResult path = grid->FindPath(p, a.target);
                    a.repathTimer = 0.0f;
                    a.pathTarget = a.target;
                    ++a.replans;
                    ++stats.pathsPlanned;
                    if (path.found) { a.path = path.points; a.pathIndex = a.path.size() > 1 ? 1 : 0; a.pathFailed = false; }
                    else { a.path.clear(); a.pathFailed = true; ++stats.pathFailures; }
                }
                if (!a.path.empty()) {
                    // advance past reached waypoints (or ones already in view beyond)
                    while (a.pathIndex + 1 < a.path.size()) {
                        const glm::vec3 w = a.path[a.pathIndex];
                        const float d = glm::length(glm::vec2(w.x - p.x, w.z - p.z));
                        if (d < std::max(a.radius, 0.3f) || grid->LineOfSight(p, a.path[a.pathIndex + 1])) ++a.pathIndex;
                        else break;
                    }
                    const glm::vec3 w = a.path[std::min(a.pathIndex, a.path.size() - 1)];
                    glm::vec3 to(w.x - p.x, 0.0f, w.z - p.z);
                    const float d = glm::length(to);
                    const bool last = a.pathIndex + 1 >= a.path.size();
                    if (last && d < a.arriveRadius) { a.arrived = true; }
                    else if (d > 1e-5f) {
                        const float slow = last ? std::min(1.0f, d / 1.2f) : 1.0f;
                        desired = to / d * a.maxSpeed * slow;
                    }
                }
            }
            // separation + time-to-collision avoidance
            glm::vec3 steer(0.0f);
            for (size_t j = 0; j < agents.size(); ++j) {
                if (j == i) continue;
                const NavAgent& b = *agents[j].agent;
                glm::vec3 x = agents[j].position - p;
                x.y = 0.0f;
                const float dist = glm::length(x);
                const float R = a.radius + b.radius;
                if (dist < R + 0.4f && dist > 1e-5f) steer -= x / dist * a.separation * (R + 0.4f - dist) / (R + 0.4f) * a.maxAcceleration;
                if (dist > 6.0f) continue;
                const glm::vec3 v = a.velocity - b.velocity;
                const float av = glm::dot(v, v), bv = glm::dot(v, x), cv = glm::dot(x, x) - R * R;
                if (av < 1e-6f || bv <= 0.0f) continue;
                const float disc = bv * bv - av * cv;
                if (disc <= 0.0f) continue;
                const float tau = cv < 0.0f ? 0.0f : (bv - std::sqrt(disc)) / av;
                const float horizon = 3.0f;
                if (tau > horizon) continue;
                glm::vec3 away = (p + a.velocity * tau) - (agents[j].position + b.velocity * tau);
                away.y = 0.0f;
                const float len = glm::length(away);
                if (len > 1e-5f) steer += away / len * a.avoidance * (horizon - tau) / (tau + 0.25f);
            }
            glm::vec3 accel = (desired - a.velocity) * 4.0f + steer;
            accel.y = 0.0f;
            const float am = glm::length(accel);
            if (am > a.maxAcceleration) accel *= a.maxAcceleration / am;
            glm::vec3 v = a.velocity + accel * dt;
            const float speed = glm::length(v);
            if (speed > a.maxSpeed) v *= a.maxSpeed / speed;
            if (a.arrived && glm::length(steer) < 1e-3f) v *= std::max(0.0f, 1.0f - 6.0f * dt);
            velocities[i] = v;
        }
        // integrate (after all agents decided: order independent)
        for (size_t i = 0; i < agents.size(); ++i) {
            NavAgent& a = *agents[i].agent;
            glm::vec3 v = velocities[i];
            const glm::vec3 p = agents[i].position;
            glm::vec3 next = p + v * dt;
            int x, z;
            auto ok = [&](const glm::vec3& q) { return grid->WorldToCell(q, x, z) && grid->IsWalkable(x, z); };
            if (!ok(next)) {
                const glm::vec3 alongX(next.x, p.y, p.z), alongZ(p.x, p.y, next.z);
                if (ok(alongX)) { next = alongX; v.z = 0.0f; }
                else if (ok(alongZ)) { next = alongZ; v.x = 0.0f; }
                else { next = p; v = glm::vec3(0.0f); }
            }
            next.y = grid->FloorAt(next) + a.heightOffset;
            a.travelled += glm::length(glm::vec2(next.x - p.x, next.z - p.z));
            a.velocity = v;
            Entity entity(&scene, agents[i].handle);
            glm::quat rotation = entity.WorldRotation();
            if (glm::length(glm::vec2(v.x, v.z)) > 0.05f) {
                const glm::quat facing = glm::quatLookAtRH(glm::normalize(glm::vec3(v.x, 0.0f, v.z)), glm::vec3(0, 1, 0));
                rotation = glm::slerp(rotation, facing, std::min(1.0f, 10.0f * dt));
            }
            if (!entity.GetParent()) {
                Transform& t = entity.LocalTransform();
                t.position = next;
                t.rotation = rotation;
                const Physics::BodyId body = entity.GetBody();
                if (body.Valid() && Physics::GetBodyType(body) != Physics::BodyType::Kinematic) Physics::SetPose(body, next, rotation);
            }
            else entity.SetWorldPose(next, rotation);
            agents[i].position = next;
            if (a.arrived) ++stats.arrived;
        }
        stats.minSeparation = FLT_MAX;
        for (size_t i = 0; i < agents.size(); ++i)
            for (size_t j = i + 1; j < agents.size(); ++j) {
                const glm::vec3 d = agents[j].position - agents[i].position;
                stats.minSeparation = std::min(stats.minSeparation, std::sqrt(d.x * d.x + d.z * d.z) - agents[i].agent->radius - agents[j].agent->radius);
            }
        if (agents.size() < 2) stats.minSeparation = 0.0f;
        g_stats = stats;
    }

    Stats GetStats() { return g_stats; }
}
