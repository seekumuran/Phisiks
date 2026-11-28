#pragma once
// Phisiks::AI: navigation from static scene geometry and crowd steering.
//
//   NavGrid  walkable-surface grid ("grid nav mesh"): every cell is probed with physics raycasts
//            (floor height + slope) and an overlap box (head room), blocked cells are dilated by
//            the agent radius, neighbours connect when the height step is climbable. Built from the
//            static / kinematic colliders of either physics backend (dynamic bodies are ignored).
//   A*       8-connected, octile heuristic, no corner cutting, then string-pulled with grid
//            line-of-sight tests (shortest any-angle path through the visible cells).
//   Crowd    NavAgent components follow their paths with arrival, separation and time-to-collision
//            avoidance between agents, kept on walkable cells and on the floor height.

#include "Phisiks/Physics/PhysicsTypes.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace Phisiks {
    class Scene;
}

namespace Phisiks::AI {
    struct NavGridDesc {
        glm::vec3 boundsMin{ -10.0f, -2.0f, -10.0f }, boundsMax{ 10.0f, 6.0f, 10.0f };
        float cellSize = 0.25f;
        float agentRadius = 0.35f;
        float agentHeight = 1.8f;
        float maxStepHeight = 0.35f;
        float maxSlopeDegrees = 40.0f;
        Physics::LayerMask layers = Physics::ALL_LAYERS;
    };

    struct PathResult {
        bool found = false;
        std::vector<glm::vec3> points;         // start .. goal (on the floor)
        float length = 0.0f;
        uint32_t expanded = 0;                 // A* nodes expanded
        uint32_t rawPoints = 0;                // cells before string pulling
    };

    class NavGrid {
    public:
        bool Build(const NavGridDesc& desc);   // needs Physics initialised; static geometry only
        bool Valid() const { return m_width > 0; }
        const NavGridDesc& Desc() const { return m_desc; }
        int Width() const { return m_width; }
        int Depth() const { return m_depth; }
        uint32_t WalkableCells() const { return m_walkableCount; }
        uint32_t Regions() const { return m_regionCount; }   // connected walkable islands

        bool IsWalkable(int x, int z) const;
        float FloorHeight(int x, int z) const;
        int Region(int x, int z) const;        // -1 if not walkable
        bool WorldToCell(const glm::vec3& p, int& x, int& z) const;
        glm::vec3 CellCenter(int x, int z) const;   // on the floor
        // Walkable cell nearest to `p` within `searchRadius` cells (false if none).
        bool NearestWalkable(const glm::vec3& p, int& x, int& z, int searchRadius = 8) const;
        bool LineOfSight(const glm::vec3& a, const glm::vec3& b) const;   // straight walk stays on connected walkable cells
        float FloorAt(const glm::vec3& p) const;    // floor height under p (nearest walkable)

        PathResult FindPath(const glm::vec3& start, const glm::vec3& goal, bool smooth = true) const;

    private:
        bool Connected(int x0, int z0, int x1, int z1) const;
        NavGridDesc m_desc;
        int m_width = 0, m_depth = 0;
        std::vector<uint8_t> m_walkable;
        std::vector<float> m_height;
        std::vector<int> m_region;
        uint32_t m_walkableCount = 0, m_regionCount = 0;
    };
}

namespace Phisiks {
    // Marks the area the scene's navigation grid covers (one per scene). Built on first use.
    struct NavigationSurface {
        AI::NavGridDesc desc;
        bool rebuild = false;                  // set to rebuild (e.g. after moving static geometry)
        // runtime
        std::shared_ptr<AI::NavGrid> grid;
        float buildMs = 0.0f;
    };

    // Moves the entity along a path to `target`. Kinematic rigid bodies are moved through their
    // transform; entities without physics are moved directly. The entity's origin is kept at
    // floor height + heightOffset.
    struct NavAgent {
        glm::vec3 target{ 0.0f };
        bool hasTarget = false;
        float maxSpeed = 2.5f;                 // m/s
        float maxAcceleration = 10.0f;         // m/s^2
        float radius = 0.35f;
        float arriveRadius = 0.25f;
        float separation = 1.5f;               // separation strength
        float avoidance = 2.0f;                // time-to-collision avoidance strength
        float heightOffset = 0.0f;
        float repathInterval = 1.0f;           // seconds (also when the target moves > 0.5 m)
        // runtime
        std::vector<glm::vec3> path;
        size_t pathIndex = 0;
        glm::vec3 velocity{ 0.0f };
        bool arrived = false;
        bool pathFailed = false;
        float repathTimer = 0.0f;
        glm::vec3 pathTarget{ 0.0f };
        uint32_t replans = 0;
        float travelled = 0.0f;
    };

    namespace AI {
        void SetTarget(NavAgent& agent, const glm::vec3& target);
        void UpdateScene(Scene& scene, float deltaTime);   // builds the grid, plans and steers agents
        NavGrid* SceneGrid(Scene& scene);                  // the NavigationSurface's grid (nullptr if none)
        struct Stats { uint32_t agents = 0, arrived = 0, pathsPlanned = 0, pathFailures = 0; float minSeparation = 0.0f; float buildMs = 0.0f; uint32_t walkableCells = 0; };
        Stats GetStats();
    }
}
