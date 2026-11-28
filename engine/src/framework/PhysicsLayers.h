#pragma once
// Physics layers used by the gameplay framework (Phisiks::Physics collision matrix: all layers
// collide by default). Queries from the player's camera ignore the player's own capsule by id.

#include "Phisiks/Physics/PhysicsTypes.h"

namespace Phisiks::PhysicsLayers {
    inline constexpr Physics::Layer WORLD = 0;    // static geometry and props
    inline constexpr Physics::Layer PLAYER = 1;   // character controllers
}
