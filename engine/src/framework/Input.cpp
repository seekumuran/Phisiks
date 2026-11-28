#include "Phisiks/Framework/Input.h"

#include "Phisiks/Core/InputState.h"

#include <glm/geometric.hpp>

namespace Phisiks {
    namespace {
        enum class Source { Key, Mouse };
        struct Binding { Source source; int code; };

        // Default bindings. Mouse codes: 0 left, 1 right.
        Binding GetBinding(Action action, int slot) {
            switch (action) {
                case Action::MoveForward:        return slot == 0 ? Binding{ Source::Key, static_cast<int>(Key::W) } : Binding{ Source::Key, static_cast<int>(Key::Up) };
                case Action::MoveBack:           return slot == 0 ? Binding{ Source::Key, static_cast<int>(Key::S) } : Binding{ Source::Key, static_cast<int>(Key::Down) };
                case Action::MoveLeft:           return slot == 0 ? Binding{ Source::Key, static_cast<int>(Key::A) } : Binding{ Source::Key, static_cast<int>(Key::Left) };
                case Action::MoveRight:          return slot == 0 ? Binding{ Source::Key, static_cast<int>(Key::D) } : Binding{ Source::Key, static_cast<int>(Key::Right) };
                case Action::Jump:               return { Source::Key, static_cast<int>(Key::Space) };
                case Action::Sprint:             return { Source::Key, static_cast<int>(Key::LeftShift) };
                case Action::Crouch:             return { Source::Key, static_cast<int>(Key::LeftControl) };
                case Action::Interact:           return { Source::Key, static_cast<int>(Key::E) };
                case Action::Primary:            return { Source::Mouse, 0 };
                case Action::Secondary:          return { Source::Mouse, 1 };
                case Action::CycleCamera:        return { Source::Key, static_cast<int>(Key::V) };
                case Action::ToggleOverlay:      return { Source::Key, static_cast<int>(Key::F1) };
                case Action::TogglePhysicsDebug: return { Source::Key, static_cast<int>(Key::F2) };
                case Action::ReloadScene:        return { Source::Key, static_cast<int>(Key::R) };
                case Action::NextScene:          return { Source::Key, static_cast<int>(Key::N) };
                case Action::Pause:              return { Source::Key, static_cast<int>(Key::Escape) };
                case Action::Quit:               return { Source::Key, static_cast<int>(Key::Q) };
                case Action::Pan:                return { Source::Mouse, 2 };
                case Action::ToggleViewLayout:   return { Source::Key, static_cast<int>(Key::F3) };
                case Action::ToggleOrbitCamera:  return { Source::Key, static_cast<int>(Key::F4) };
                default:                         return { Source::Key, -1 };
            }
        }

        bool HasSecondSlot(Action action) {
            return action == Action::MoveForward || action == Action::MoveBack ||
                   action == Action::MoveLeft || action == Action::MoveRight;
        }

        bool HardwareDown(Action action) {
            for (int slot = 0; slot < (HasSecondSlot(action) ? 2 : 1); ++slot) {
                const Binding binding = GetBinding(action, slot);
                if (binding.code < 0) continue;
                const InputState& state = GetInputState();
                if (binding.source == Source::Key && state.IsDown(binding.code)) return true;
                if (binding.source == Source::Mouse && state.IsDown(static_cast<MouseButton>(binding.code))) return true;
            }
            return false;
        }
    }

    void InputSystem::Update(bool windowHasFocus) {
        const bool useHardware = m_hardwareEnabled && windowHasFocus;
        m_downLastFrame = m_down;
        for (size_t i = 0; i < COUNT; ++i) {
            const Action action = static_cast<Action>(i);
            m_down[i] = (useHardware && HardwareDown(action)) || m_injectedHold[i] || m_injectedPress[i];
            m_injectedPress[i] = false;
        }
        m_mouseDelta = m_injectedMouse;
        m_scroll = m_injectedScroll;
        if (useHardware) {
            m_mouseDelta += glm::vec2(GetInputState().CursorDelta());
            m_scroll += float(GetInputState().ScrollDelta().y);
        }
        m_injectedMouse = glm::vec2(0.0f);
        m_injectedScroll = 0.0f;
    }

    bool InputSystem::Down(Action action) const {
        return m_down[static_cast<size_t>(action)];
    }

    bool InputSystem::Pressed(Action action) const {
        const size_t i = static_cast<size_t>(action);
        return m_down[i] && !m_downLastFrame[i];
    }

    glm::vec2 InputSystem::MoveAxis() const {
        glm::vec2 axis(0.0f);
        if (Down(Action::MoveForward)) axis.y += 1.0f;
        if (Down(Action::MoveBack))    axis.y -= 1.0f;
        if (Down(Action::MoveRight))   axis.x += 1.0f;
        if (Down(Action::MoveLeft))    axis.x -= 1.0f;
        const float lengthSquared = glm::dot(axis, axis);
        return lengthSquared > 1.0f ? axis / glm::sqrt(lengthSquared) : axis;
    }

    void InputSystem::InjectScroll(float notches) { m_injectedScroll += notches; }

    void InputSystem::InjectHold(Action action, bool down) {
        m_injectedHold[static_cast<size_t>(action)] = down;
    }

    void InputSystem::InjectPress(Action action) {
        m_injectedPress[static_cast<size_t>(action)] = true;
    }

    void InputSystem::InjectMouseDelta(glm::vec2 delta) {
        m_injectedMouse += delta;
    }

    void InputSystem::ClearInjected() {
        m_injectedHold.fill(false);
        m_injectedPress.fill(false);
        m_injectedMouse = glm::vec2(0.0f);
    }
}
