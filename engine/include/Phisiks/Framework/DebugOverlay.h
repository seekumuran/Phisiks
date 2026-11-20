#pragma once

namespace Phisiks {
    struct GameContext;

    namespace DebugOverlay {
        // Text overlay (stats, player, camera, target, events, controls) + crosshair.
        void Draw(GameContext& ctx);
        // World-space physics visualisation: entity bounds (green = awake, grey = asleep/static),
        // the player capsule bounds and the interaction ray hit.
        void DrawPhysicsDebug(GameContext& ctx);
    }
}
