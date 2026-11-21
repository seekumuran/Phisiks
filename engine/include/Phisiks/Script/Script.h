#pragma once
// Phisiks::Script: embedded Lua 5.4 (MIT licence) for gameplay without recompiling C++.
//
// Each Script component runs its chunk in its own environment (globals are per script, the
// engine API is shared). Callbacks it may define:
//   on_start(self)                       once, before the first update
//   on_update(self, dt)                  every frame (variable dt)
//   on_fixed_update(self, dt)            every fixed step, before physics
//   on_collision(self, other, speed)     collision enter with another entity (speed = impact m/s)
//   on_trigger(self, other, entered)     trigger enter (true) / exit (false)
// `self.entity` is this entity; `self` is the script's environment table (keeps state).
//
// API (vectors are returned as x, y, z):
//   entity: name() valid() position() set_position(x,y,z) rotation() (quaternion x,y,z,w)
//           euler() set_euler(x,y,z) (degrees) rotate(ax,ay,az,degrees) scale() set_scale(x,y,z)
//           velocity() set_velocity(x,y,z) impulse(x,y,z) force(x,y,z) torque(x,y,z) mass()
//           destroy() parent() light_intensity() set_light_intensity(v) set_color(r,g,b) (material
//           base colour of its MeshRenderer) set_visible(bool) play_sound(clip)
//           joint_motor(speed [, maxForce]) joint_state() -> position, speed (its Joint component)
//   scene:  find(name) spawn(name, mesh, x,y,z [, "box"|"sphere", mass]) time() frame()
//           raycast(ox,oy,oz, dx,dy,dz, max) -> entity|nil, x,y,z, distance  gravity(x,y,z)
//           event(text) (studio / overlay event log)
//   input:  down(action) pressed(action)   actions: "forward" "back" "left" "right" "jump"
//           "sprint" "crouch" "interact" "primary" "secondary"
//   log(...) print(...)
// Sandboxed: base (without dofile / loadfile / load of binary chunks), table, string, math, utf8,
// coroutine; no io / os / package / debug. A runaway script is stopped after 200 ms per call.

#include <cstdint>
#include <string>

namespace Phisiks {
    class Scene;
    struct GameContext;

    struct Script {
        std::string source;                    // inline Lua source, or
        std::string file;                      // asset path (re-run when the file changes on disk)
        bool enabled = true;
        // runtime
        bool started = false;
        std::string error;                     // last error (the script is disabled until reloaded)
        int reference = 0;                     // environment table in the VM
        uint64_t sourceStamp = 0;
    };

    namespace Scripting {
        void Update(Scene& scene, GameContext& ctx, float deltaTime);        // load / start / on_update
        void FixedUpdate(Scene& scene, GameContext& ctx, float fixedDeltaTime);
        void DispatchPhysicsEvents(Scene& scene, GameContext& ctx);         // after the physics steps
        void Shutdown();                                                     // drops the VM
        // Runs code in a scratch environment of the current scene's VM (console / tests).
        bool Run(Scene& scene, GameContext& ctx, const std::string& code, std::string* error = nullptr);
        void Reload(Scene& scene);                                           // re-run every Script's source (keeps state)
        struct Stats { uint32_t scripts = 0, running = 0, errors = 0; uint64_t callbacks = 0; float ms = 0.0f; uint32_t reloads = 0; std::string lastError; };
        Stats GetStats();
    }
}
