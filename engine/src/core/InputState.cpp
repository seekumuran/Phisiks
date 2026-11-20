#include "Phisiks/Core/InputState.h"

namespace Phisiks {

    void InputState::OnKey(int key, bool down) {
        if (!ValidKey(key)) return;
        if (down && !m_keyDown[key]) m_keyPressedPending.set(key);
        if (!down && m_keyDown[key]) m_keyReleasedPending.set(key);
        m_keyDown[key] = down;
    }

    void InputState::OnMouseButton(int button, bool down) {
        if (!ValidButton(button)) return;
        if (down && !m_buttonDown[button]) m_buttonPressedPending.set(button);
        if (!down && m_buttonDown[button]) m_buttonReleasedPending.set(button);
        m_buttonDown[button] = down;
    }

    void InputState::OnCursorPosition(double x, double y) {
        const glm::dvec2 position(x, y);
        if (m_cursorKnown) m_cursorDeltaPending += position - m_cursor;
        m_cursor = position;
        m_cursorKnown = true;
    }

    void InputState::OnScroll(double dx, double dy) {
        m_scrollPending += glm::dvec2(dx, dy);
    }

    void InputState::OnFocusLost() {
        for (int key = 0; key < KEY_COUNT; ++key) {
            if (m_keyDown[key]) OnKey(key, false);
        }
        for (int button = 0; button < BUTTON_COUNT; ++button) {
            if (m_buttonDown[button]) OnMouseButton(button, false);
        }
        ResetCursorTracking();
    }

    void InputState::ResetCursorTracking() {
        m_cursorKnown = false;
    }

    void InputState::NewFrame() {
        m_keyPressed = m_keyPressedPending;
        m_keyReleased = m_keyReleasedPending;
        m_buttonPressed = m_buttonPressedPending;
        m_buttonReleased = m_buttonReleasedPending;
        m_frameCursorDelta = m_cursorDeltaPending;
        m_frameScroll = m_scrollPending;

        m_keyPressedPending.reset();
        m_keyReleasedPending.reset();
        m_buttonPressedPending.reset();
        m_buttonReleasedPending.reset();
        m_cursorDeltaPending = glm::dvec2(0.0);
        m_scrollPending = glm::dvec2(0.0);
    }

    bool InputState::IsDown(int key) const       { return ValidKey(key) && m_keyDown[key]; }
    bool InputState::WasPressed(int key) const   { return ValidKey(key) && m_keyPressed[key]; }
    bool InputState::WasReleased(int key) const  { return ValidKey(key) && m_keyReleased[key]; }

    bool InputState::IsDown(MouseButton button) const      { return m_buttonDown[static_cast<int>(button)]; }
    bool InputState::WasPressed(MouseButton button) const  { return m_buttonPressed[static_cast<int>(button)]; }
    bool InputState::WasReleased(MouseButton button) const { return m_buttonReleased[static_cast<int>(button)]; }

    InputState& GetInputState() {
        static InputState state;
        return state;
    }
}
