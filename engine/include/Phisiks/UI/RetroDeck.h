#pragma once
// "Retro deck" studio skin, P11 look: a dark 1990s retro hardware / engineering console for the
// Dear ImGui studio and the showcase demos (original artwork: everything is drawn procedurally,
// no third-party skin assets).
//
//   matte obsidian-black panels (#1B1D26 / #12131A) and dark gunmetal faces (#252834) with
//   chiseled 3D bevels (dark-steel #3E4356 top / left highlights, pure black #07080B bottom / right
//   shadows), square corners, gunmetal title bars with amber double stripes and silver-white
//   titles, a bevelled top menu bar, etched "group box" control groups (Joint Jog,
//   Cartesian Jog, Shader Passes, ...), tactile bevelled push buttons with green / amber LEDs,
//   recessed pitch-black LCD readouts in phosphor green (#00FF41) or amber (#FFBE0B), black
//   "playlist" lists, segmented green -> yellow -> red LED meters, gradient sliders with metallic
//   thumbs, and transport buttons (step back, play, pause, stop, step forward).
//
// Call between UI::BeginFrame / EndFrame (inside IGame::OnGui). Plain types only, so game code
// does not need ImGui headers for these widgets (it may mix them with <imgui.h> calls).
#include <cstdint>

namespace Phisiks::UI::Retro {
    void ApplyTheme();                          // UI::Init applies it

    // Panel with the bevelled frame and the amber double-striped title "=== TITLE ===".
    bool BeginPanel(const char* title, float x, float y, float width, float height = 0.0f, bool* open = nullptr);
    void EndPanel();

    // Top menu bar of the console (an ImGui main menu bar with a raised bevel and a title plate).
    // Returns false when it is not drawn (then do not call EndMenuBar). MenuBarHeight() is its height.
    bool BeginMenuBar(const char* title = nullptr);
    void EndMenuBar();
    float MenuBarHeight();

    // Etched group box ("Joint Jog", "Cartesian Jog", ...): content between BeginGroup / EndGroup
    // is framed by a two-tone etched line with the title cut into the top edge.
    void BeginGroup(const char* title, float width = 0.0f);
    void EndGroup();

    // Recessed black LCD with glowing phosphor-green (or amber) digits (unlit "8" segments ghosted behind).
    void Lcd(const char* bigText, const char* smallText = nullptr, float height = 0.0f, bool amber = false);
    // One-line readout: label on the panel, value in a small recessed LCD field on the right.
    void LcdField(const char* label, const char* value, bool amber = false, float valueWidth = 0.0f);

    // Segmented LED bar graph (green -> yellow -> red). fraction 0..1; valueText drawn on the right.
    void LedMeter(const char* label, float fraction, const char* valueText = nullptr);

    // Spectrum-analyser style history (bars coloured by height), values 0..1, newest last.
    void Analyzer(const char* id, const float* values, int count, int offset, float height = 48.0f);

    // Gradient slider (green -> amber -> red track), chunky metallic thumb, value in an LCD field.
    // Vertical sliders are laid out side by side with SameLine(); the label goes below.
    bool EqSlider(const char* label, float* value, float minValue, float maxValue, const char* format = "%.2f",
                  bool vertical = false, float length = 0.0f);
    bool EqSliderInt(const char* label, int* value, int minValue, int maxValue, bool vertical = false, float length = 0.0f);

    enum class Glyph { StepBack, Play, Pause, Stop, StepForward, Record, Eject };
    bool TransportButton(const char* id, Glyph glyph, bool lit = false, const char* tooltip = nullptr);

    // Chunky bevelled push button; `led` >= 0 draws a status LED (0 off, 1 green, 2 amber).
    bool Button(const char* label, float width = 0.0f, int led = -1);
    bool LedToggle(const char* label, bool* value);          // button + green LED while on

    // "Playlist" list: recessed black box, numbered green rows, right-aligned info, amber/white selection.
    bool BeginPlaylist(const char* id, float height);
    bool PlaylistRow(int number, const char* text, const char* rightText, bool selected);   // true when clicked
    void EndPlaylist();
    // Colours for a tree drawn inside a playlist box (hierarchy): push before, pop after.
    void PushPlaylistTreeStyle();
    void PopPlaylistTreeStyle();

    // Verification counters for the last UI frame (tests).
    struct FrameStats {
        int panels = 0, lcds = 0, meters = 0, litSegments = 0, eqSliders = 0, transportButtons = 0, playlistRows = 0;
        int groups = 0, lcdFields = 0, menuBars = 0;
        float lcdRect[4] = { 0, 0, 0, 0 };      // x, y, w, h of the last LCD (window pixels, top-left origin)
        float panelRects[8][4] = {};             // first panels drawn
    };
    const FrameStats& LastFrameStats();
    void ResetFrameStats();                     // UI::BeginFrame calls it

    // LCD colours (RGBA 0..1): phosphor green and amber, for text drawn on black.
    inline constexpr float PHOSPHOR[4] = { 0.0f, 1.0f, 0.255f, 1.0f };
    inline constexpr float AMBER[4] = { 1.0f, 0.745f, 0.043f, 1.0f };
    inline constexpr float GOLD[4] = { 0.95f, 0.78f, 0.30f, 1.0f };   // P9 name of the amber accent
    // Text on the dark console panels: headings / readouts in warm amber (#FFBE0B), body text in
    // crisp silver-white (#E2E6F0).
    inline constexpr float HEADING[4] = { 1.0f, 0.745f, 0.043f, 1.0f };
    inline constexpr float READOUT[4] = { 1.0f, 0.745f, 0.043f, 1.0f };
    inline constexpr float INK[4] = { 0.886f, 0.902f, 0.941f, 1.0f };
}
