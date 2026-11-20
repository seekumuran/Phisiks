#pragma once
// Gameplay components used by the generic gameplay services (Phisiks/Framework/Gameplay.h).

#include "Phisiks/Scene/Scene.h"

#include <functional>
#include <string>

namespace Phisiks {
    struct GameContext;

    using EntityCallback = std::function<void(GameContext& ctx, Entity self, Entity instigator)>;

    struct Health {
        float current = 100.0f;
        float max = 100.0f;
        bool dead = false;
        EntityCallback onDeath;      // default when empty: the entity is destroyed
    };

    struct Interactable {
        std::string prompt;          // shown by the debug overlay ("[E] <prompt>")
        EntityCallback onInteract;
    };
}
