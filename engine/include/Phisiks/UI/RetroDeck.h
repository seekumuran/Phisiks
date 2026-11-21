#pragma once
// "Retro deck" studio skin: a late-90s desktop media-player look for the Dear ImGui studio
// (original artwork: everything is drawn procedurally, no third-party skin assets).
//
//   brushed charcoal-slate panels (#2B2B3B / #38384C) with chiseled 3D bevels, square corners,
//   gold double-striped title bars, chunky bevelled buttons with green / amber LEDs,
//   recessed pitch-black LCD readouts and "playlist" lists in phosphor green (#00FF41),
//   segmented green -> yellow -> red LED meters, equaliser-style gradient sliders with metallic
//   thumbs, and transport buttons (step back, play, pause, stop, step forward).
//
// Call between UI::BeginFrame / EndFrame (inside IGame::OnGui). Plain types only, so game code
// does not need ImGui headers for these widgets (it may mix them with <imgui.h> calls).
#include <cstdint>

namespace Phisiks::UI::Retro {
    void ApplyTheme();                          // UI::Init applies it

    // Panel with the bevelled frame and the gold double-striped title "=== TITLE ===".
    bool BeginPanel(const char* title, float x, float y, float width, float height = 0.0f, bool* open = nullptr);
    void EndPanel();

    // Recessed black LCD with glowing phosphor-green digits (unlit "8" segments ghosted behind).
    void Lcd(const char* bigText, const char* smallText = nullptr, float height = 0.0f);

    // Segmented LED bar graph (green -> yellow -> red). fraction 0..1; valueText drawn on the right.
    void LedMeter(const char* label, float fraction, const char* valueText = nullptr);

    // Spectrum-analyser style history (bars coloured by height), values 0..1, newest last.
    void Analyzer(const char* id, const float* values, int count, int offset, float height = 48.0f);

    // Equaliser slider: gradient track (green -> amber -> red), chunky metallic thumb.
    // Vertical sliders are laid out side by side with SameLine(); the label goes below.
    bool EqSlider(const char* label, float* value, float minValue, float maxValue, const char* format = "%.2f",
                  bool vertical = false, float length = 0.0f);
    bool EqSliderInt(const char* label, int* value, int minValue, int maxValue, bool vertical = false, float length = 0.0f);

    enum class Glyph { StepBack, Play, Pause, Stop, StepForward, Record, Eject };
    bool TransportButton(const char* id, Glyph glyph, bool lit = false, const char* tooltip = nullptr);

    // Chunky bevelled push button; `led` >= 0 draws a status LED (0 off, 1 green, 2 amber).
    bool Button(const char* label, float width = 0.0f, int led = -1);
    bool LedToggle(const char* label, bool* value);          // button + green LED while on

    // "Playlist" list: recessed black box, numbered green rows, right-aligned info, gold/white selection.
    bool BeginPlaylist(const char* id, float height);
    bool PlaylistRow(int number, const char* text, const char* rightText, bool selected);   // true when clicked
    void EndPlaylist();
    // Colours for a tree drawn inside a playlist box (hierarchy): push before, pop after.
    void PushPlaylistTreeStyle();
    void PopPlaylistTreeStyle();

    // Verification counters for the last UI frame (tests).
    struct FrameStats {
        int panels = 0, lcds = 0, meters = 0, litSegments = 0, eqSliders = 0, transportButtons = 0, playlistRows = 0;
        float lcdRect[4] = { 0, 0, 0, 0 };      // x, y, w, h of the last LCD (window pixels, top-left origin)
        float panelRects[8][4] = {};             // first panels drawn
    };
    const FrameStats& LastFrameStats();
    void ResetFrameStats();                     // UI::BeginFrame calls it

    // Phosphor-green / gold reference colours (RGBA 0..1) for game-drawn text.
    inline constexpr float PHOSPHOR[4] = { 0.0f, 1.0f, 0.255f, 1.0f };
    inline constexpr float GOLD[4] = { 0.95f, 0.78f, 0.30f, 1.0f };
}
