#pragma once
// Raw keyboard/mouse state, fed by platform events and latched once per frame.
//
// Event-driven (not polled), so a key pressed and released within a single frame still reports
// WasPressed() and WasReleased() for that frame. Has no OS dependency and can be unit-tested
// by feeding events directly.

#include <glm/vec2.hpp>

#include <array>
#include <bitset>
#include <cstdint>

namespace Phisiks {

    // Key codes. Numeric values follow the GLFW key tokens so platform events map directly.
    enum class Key : int16_t {
        Unknown = -1,
        Space = 32, Apostrophe = 39, Comma = 44, Minus = 45, Period = 46, Slash = 47,
        Num0 = 48, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
        Semicolon = 59, Equal = 61,
        A = 65, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
        LeftBracket = 91, Backslash = 92, RightBracket = 93, GraveAccent = 96,
        Escape = 256, Enter, Tab, Backspace, Insert, Delete, Right, Left, Down, Up,
        PageUp = 266, PageDown, Home, End,
        CapsLock = 280, ScrollLock, NumLock, PrintScreen, Pause,
        F1 = 290, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
        Keypad0 = 320, Keypad1, Keypad2, Keypad3, Keypad4, Keypad5, Keypad6, Keypad7, Keypad8, Keypad9,
        KeypadDecimal = 330, KeypadDivide, KeypadMultiply, KeypadSubtract, KeypadAdd, KeypadEnter, KeypadEqual,
        LeftShift = 340, LeftControl, LeftAlt, LeftSuper, RightShift, RightControl, RightAlt, RightSuper, Menu
    };

    enum class MouseButton : uint8_t { Left = 0, Right = 1, Middle = 2, Button4 = 3, Button5 = 4 };

    class InputState {
    public:
        static constexpr int KEY_COUNT = 512;
        static constexpr int BUTTON_COUNT = 8;

        // Event intake (called by the platform layer, or by tests).
        void OnKey(int key, bool down);
        void OnMouseButton(int button, bool down);
        void OnCursorPosition(double x, double y);
        void OnScroll(double dx, double dy);
        void OnFocusLost();       // releases everything that is held (prevents stuck keys)
        void ResetCursorTracking(); // next cursor event sets the position without producing a delta

        // Latches this frame's edges, deltas and scroll. Call once per frame after polling events.
        void NewFrame();

        bool IsDown(Key key) const              { return IsDown(static_cast<int>(key)); }
        bool WasPressed(Key key) const          { return WasPressed(static_cast<int>(key)); }
        bool WasReleased(Key key) const         { return WasReleased(static_cast<int>(key)); }
        bool IsDown(int key) const;
        bool WasPressed(int key) const;
        bool WasReleased(int key) const;

        bool IsDown(MouseButton button) const;
        bool WasPressed(MouseButton button) const;
        bool WasReleased(MouseButton button) const;

        glm::dvec2 CursorPosition() const       { return m_cursor; }
        glm::dvec2 CursorDelta() const          { return m_frameCursorDelta; }   // this frame
        glm::dvec2 ScrollDelta() const          { return m_frameScroll; }        // this frame

    private:
        static bool ValidKey(int key)       { return key >= 0 && key < KEY_COUNT; }
        static bool ValidButton(int button) { return button >= 0 && button < BUTTON_COUNT; }

        // Live state (changes as events arrive).
        std::bitset<KEY_COUNT> m_keyDown;
        std::bitset<KEY_COUNT> m_keyPressedPending, m_keyReleasedPending;
        std::bitset<BUTTON_COUNT> m_buttonDown;
        std::bitset<BUTTON_COUNT> m_buttonPressedPending, m_buttonReleasedPending;
        glm::dvec2 m_cursor{ 0.0 };
        glm::dvec2 m_cursorDeltaPending{ 0.0 };
        glm::dvec2 m_scrollPending{ 0.0 };
        bool m_cursorKnown = false;

        // Latched per-frame state (stable between NewFrame calls).
        std::bitset<KEY_COUNT> m_keyPressed, m_keyReleased;
        std::bitset<BUTTON_COUNT> m_buttonPressed, m_buttonReleased;
        glm::dvec2 m_frameCursorDelta{ 0.0 };
        glm::dvec2 m_frameScroll{ 0.0 };
    };

    // The engine-wide device state, fed by the platform window.
    InputState& GetInputState();
}
