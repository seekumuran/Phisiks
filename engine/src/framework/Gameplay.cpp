#include "Phisiks/Framework/Gameplay.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Core/Log.h"
#include "framework/PhysicsLayers.h"

#include "Phisiks/Physics/Physics.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <sstream>

namespace Phisiks {
    const char* ToString(GameState state) {
        switch (state) {
            case GameState::Booting:      return "Booting";
            case GameState::Loading:      return "Loading";
            case GameState::Running:      return "Running";
            case GameState::Paused:       return "Paused";
            case GameState::ShuttingDown: return "ShuttingDown";
        }
        return "?";
    }

    void GameContext::PushEvent(const std::string& message) {
        Log::Info() << "[event] " << message;
        eventLog.push_back(message);
        while (eventLog.size() > 6) eventLog.pop_front();
    }

    Scene* GameContext::CurrentScene() const {
        return scenes ? scenes->Current() : nullptr;
    }

    namespace Gameplay {
        TraceHit Trace(GameContext& ctx, const glm::vec3& origin, const glm::vec3& direction, float range) {
            TraceHit result;
            const float length = glm::length(direction);
            if (length < 1e-6f) return result;

            Physics::QueryOptions options;
            options.ignoreCharacter = ctx.player.GetCharacter();
            const auto ray = Physics::Raycast(origin, direction / length, range, options);
            if (!ray) return result;

            result.hit = true;
            result.point = ray->point;
            result.normal = ray->normal;
            result.distance = ray->distance;
            if (Scene* scene = ctx.CurrentScene()) result.entity = scene->FromBody(ray->body);
            return result;
        }

        TraceHit TraceFromCamera(GameContext& ctx, float range) {
            return Trace(ctx, ctx.camera.GetPosition(), ctx.camera.GetForward(), range);
        }

        bool Interact(GameContext& ctx, Entity target, Entity instigator) {
            Interactable* interactable = target.TryGet<Interactable>();
            if (!interactable || !interactable->onInteract) return false;
            if (Scene* scene = target.GetScene(); scene && scene->IsPendingDestroy(target)) return false;
            EntityCallback callback = interactable->onInteract;   // copy: the callback may add/remove components
            callback(ctx, target, instigator);
            return true;
        }

        DamageEvent ApplyDamage(GameContext& ctx, Entity target, float amount, Entity instigator) {
            DamageEvent event;
            event.target = target;
            event.instigator = instigator;
            event.amount = amount;

            Health* health = target.TryGet<Health>();
            if (!health || health->dead || amount <= 0.0f || target.GetScene()->IsPendingDestroy(target)) return event;

            health->current = std::max(0.0f, health->current - amount);
            event.healthAfter = health->current;

            std::ostringstream message;
            message << target.GetName() << " took " << amount << " damage (" << health->current << "/" << health->max << ")";

            if (health->current <= 0.0f) {
                health->dead = true;
                event.killed = true;
                message << " -> destroyed";
                ctx.PushEvent(message.str());
                EntityCallback onDeath = health->onDeath;
                if (onDeath) onDeath(ctx, target, instigator);
                else target.Destroy();
                return event;
            }

            ctx.PushEvent(message.str());
            return event;
        }

        void Heal(GameContext&, Entity target, float amount) {
            Health* health = target.TryGet<Health>();
            if (!health || health->dead) return;
            health->current = std::min(health->max, health->current + amount);
        }

        Entity SpawnPlayer(GameContext& ctx, const glm::vec3& feetPosition, const PlayerSettings* settings) {
            Scene* scene = ctx.CurrentScene();
            const PlayerSettings resolved = settings ? *settings : PlayerSettings();
            if (!scene || !ctx.player.Spawn(feetPosition, resolved)) return {};

            const float height = resolved.capsuleHeight + 2.0f * resolved.capsuleRadius;
            Entity body = scene->CreateEntity("PlayerBody");
            Transform& t = body.Get<Transform>();
            t.position = feetPosition + glm::vec3(0.0f, height * 0.5f, 0.0f);
            t.scale = glm::vec3(resolved.capsuleRadius * 2.0f, height, resolved.capsuleRadius * 2.0f);
            MeshRenderer& renderer = body.Add<MeshRenderer>();   // render only: the character controller is the collider
            renderer.mesh = "Cube";
            renderer.material = "Plastic";
            ctx.player.SetBodyEntity(body);
            UpdatePlayerBody(ctx);
            return body;
        }

        void UpdatePlayerBody(GameContext& ctx) {
            Entity body = ctx.player.GetBodyEntity();
            // The handle may belong to a scene that was already unloaded: never touch it then.
            if (!ctx.player.IsSpawned() || body.GetScene() != ctx.CurrentScene() || !body.Valid()) return;
            const PlayerSettings& s = ctx.player.GetSettings();
            const float height = s.capsuleHeight + 2.0f * s.capsuleRadius;
            body.SetWorldPose(ctx.player.GetFeetPosition() + glm::vec3(0.0f, height * 0.5f, 0.0f),
                              glm::angleAxis(ctx.camera.GetYaw(), glm::vec3(0.0f, 1.0f, 0.0f)));
            body.Get<MeshRenderer>().visible = ctx.camera.GetMode() != CameraMode::FirstPerson;
        }

        void UpdatePlayerActions(GameContext& ctx, const PlayerActionSettings& settings) {
            const bool interact = ctx.input.Pressed(Action::Interact);
            const bool primary = ctx.input.Pressed(Action::Primary);
            if (!interact && !primary) return;

            const TraceHit hit = TraceFromCamera(ctx, settings.range);
            if (!hit.hit || !hit.entity.Valid()) return;
            Entity target = hit.entity;
            const Entity instigator = ctx.player.GetBodyEntity();

            if (interact) Interact(ctx, target, instigator);

            if (primary) {
                const Rigidbody* rigidbody = target.TryGet<Rigidbody>();
                if (rigidbody && rigidbody->type == Physics::BodyType::Dynamic) {
                    // Push horizontally with a slight lift: a downward component would mostly press the
                    // body into the ground (raising friction) instead of moving it.
                    glm::vec3 direction = ctx.camera.GetForward();
                    direction.y = 0.0f;
                    if (glm::length(direction) < 1e-4f) direction = glm::vec3(0.0f, 0.0f, -1.0f);
                    direction = glm::normalize(direction) + glm::vec3(0.0f, settings.pushLift, 0.0f);
                    target.AddImpulseAtPoint(direction * settings.pushImpulse, target.WorldPosition());
                }
                ApplyDamage(ctx, target, settings.damage, instigator);
            }
        }
    }
}
