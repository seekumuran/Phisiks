#pragma once
// Action-based input over Phisiks::InputState. Gameplay code asks for actions, never raw keys.
// Synthetic input (Inject*) lets automated tests drive the same code paths deterministically;
// hardware input can be disabled so stray keystrokes cannot interfere with a test run.

#include <glm/vec2.hpp>

#include <array>
#include <cstdint>

namespace Phisiks {
    enum class Action : uint8_t {
        MoveForward, MoveBack, MoveLeft, MoveRight,
        Jump, Sprint, Crouch,
        Interact,          // E
        Primary,           // left mouse
        Secondary,         // right mouse
        CycleCamera,       // V
        ToggleOverlay,     // F1
        TogglePhysicsDebug,// F2
        ReloadScene,       // R
        NextScene,         // N
        Pause,             // Escape
        Quit,              // Q (only honoured while paused)
        Pan,               // middle mouse (orbit camera)
        ToggleViewLayout,  // F3 (single view / quad view)
        ToggleOrbitCamera, // F4
        Count
    };

    class InputSystem {
    public:
        void Update(bool windowHasFocus);   // once per frame, after the platform latched InputState

        bool Down(Action action) const;
        bool Pressed(Action action) const;  // went down this frame
        glm::vec2 MouseDelta() const { return m_mouseDelta; }
        float Scroll() const { return m_scroll; }                       // wheel notches this frame (+ = away from user)
        glm::vec2 MoveAxis() const;         // x = right, y = forward, length <= 1

        void SetHardwareEnabled(bool enabled) { m_hardwareEnabled = enabled; }
        bool HardwareEnabled() const { return m_hardwareEnabled; }

        // Synthetic input (tests / replays). Held actions stay down until released.
        void InjectHold(Action action, bool down);
        void InjectPress(Action action);           // down for exactly one frame
        void InjectMouseDelta(glm::vec2 delta);    // consumed next Update
        void InjectScroll(float notches);          // consumed next Update
        void ClearInjected();

    private:
        static constexpr size_t COUNT = static_cast<size_t>(Action::Count);
        std::array<bool, COUNT> m_down{};
        std::array<bool, COUNT> m_downLastFrame{};
        std::array<bool, COUNT> m_injectedHold{};
        std::array<bool, COUNT> m_injectedPress{};
        glm::vec2 m_mouseDelta{ 0.0f };
        glm::vec2 m_injectedMouse{ 0.0f };
        float m_scroll = 0.0f;
        float m_injectedScroll = 0.0f;
        bool m_hardwareEnabled = true;
    };
}
