#pragma once

namespace Phisiks {
    struct GameContext;

    namespace DebugOverlay {
        // Text overlay (stats, player, camera, target, events, controls) + crosshair. The
        // telemetry text block is skipped while the studio panels are shown (UI::PanelsVisible),
        // so no bitmap text sits behind them.
        void Draw(GameContext& ctx);
        // World-space physics visualisation: entity bounds (green = awake, grey = asleep/static),
        // the player capsule bounds and the interaction ray hit.
        void DrawPhysicsDebug(GameContext& ctx);
        // Lines of the telemetry text block drawn last frame (0 while hidden).
        int LastTelemetryLineCount();
    }
}
