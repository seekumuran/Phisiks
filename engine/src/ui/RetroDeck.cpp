// Retro deck editor skin, dark retro hardware-console look (see Phisiks/UI/RetroDeck.h). Every
// element is drawn with ImGui draw lists: bevels are two-tone 1 px lines (dark-steel #3E4356
// top/left, pure black #07080B bottom/right for raised parts, the reverse for recessed ones),
// etched group frames are a black groove with a dark-steel ridge, gradients are multi-colour
// rectangles. No bright surfaces: obsidian / gunmetal panels, silver-white + amber text, black LCDs.
#include "Phisiks/UI/RetroDeck.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace Phisiks::UI::Retro {
    namespace {
        FrameStats g_current, g_last;

        // Palette: obsidian / gunmetal console, dark-steel bevels, silver-white + amber text, black LCDs.
        constexpr ImU32 PANEL       = IM_COL32(0x1B, 0x1D, 0x26, 0xFF);
        constexpr ImU32 TITLE_TOP   = IM_COL32(0x25, 0x28, 0x34, 0xFF);
        constexpr ImU32 TITLE_BOT   = IM_COL32(0x12, 0x13, 0x1A, 0xFF);
        constexpr ImU32 STRIPE      = IM_COL32(0xFF, 0xBE, 0x0B, 0xFF);
        constexpr ImU32 STRIPE_DIM  = IM_COL32(0x80, 0x5E, 0x08, 0xFF);
        constexpr ImU32 BEVEL_LIGHT = IM_COL32(0x3E, 0x43, 0x56, 0xFF);
        constexpr ImU32 BEVEL_MID   = IM_COL32(0x25, 0x28, 0x34, 0xFF);
        constexpr ImU32 BEVEL_DARK  = IM_COL32(0x07, 0x08, 0x0B, 0xFF);
        constexpr ImU32 INK_COL     = IM_COL32(0xE2, 0xE6, 0xF0, 0xFF);
        constexpr ImU32 HEADING_COL = IM_COL32(0xFF, 0xBE, 0x0B, 0xFF);
        constexpr ImU32 ACCENT      = IM_COL32(0xFF, 0xBE, 0x0B, 0xFF);
        constexpr ImU32 GREEN       = IM_COL32(0x00, 0xFF, 0x41, 0xFF);
        constexpr ImU32 GREEN_GLOW  = IM_COL32(0x00, 0xFF, 0x41, 0x40);
        constexpr ImU32 GREEN_GHOST = IM_COL32(0x00, 0x3A, 0x12, 0xFF);
        constexpr ImU32 AMBER_COL   = IM_COL32(0xFF, 0xBE, 0x0B, 0xFF);
        constexpr ImU32 AMBER_GLOW  = IM_COL32(0xFF, 0xBE, 0x0B, 0x40);
        constexpr ImU32 AMBER_GHOST = IM_COL32(0x3C, 0x2A, 0x00, 0xFF);
        constexpr ImU32 BLACK       = IM_COL32(0x00, 0x00, 0x00, 0xFF);
        constexpr ImU32 SELECT_BG   = IM_COL32(0x20, 0x3C, 0x78, 0xFF);
        constexpr ImU32 HOVER_BG    = IM_COL32(0x0C, 0x2C, 0x16, 0xFF);
        constexpr ImU32 WHITE       = IM_COL32(0xF4, 0xF4, 0xFA, 0xFF);
        constexpr ImU32 METAL_HI    = IM_COL32(0x5A, 0x60, 0x74, 0xFF);
        constexpr ImU32 METAL_LO    = IM_COL32(0x2A, 0x2D, 0x38, 0xFF);
        constexpr ImU32 METAL_ACTIVE = IM_COL32(0x6E, 0x75, 0x90, 0xFF);

        ImVec4 V(int r, int g, int b, int a = 255) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }
        ImVec2 Add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }

        // Raised (or, `sunken`, recessed) 3D bevel around a rectangle.
        void Bevel(ImDrawList* dl, ImVec2 a, ImVec2 b, bool sunken, int thickness = 1) {
            for (int i = 0; i < thickness; ++i) {
                const float o = float(i);
                // outer ring: full light / dark; inner rings: mid tone on the shadow side
                const ImU32 lightCol = sunken ? (i == 0 ? BEVEL_MID : BEVEL_DARK) : BEVEL_LIGHT;
                const ImU32 darkCol = sunken ? BEVEL_LIGHT : (i == 0 ? BEVEL_DARK : BEVEL_MID);
                dl->AddLine(ImVec2(a.x + o, a.y + o), ImVec2(b.x - o, a.y + o), lightCol);          // top
                dl->AddLine(ImVec2(a.x + o, a.y + o), ImVec2(a.x + o, b.y - o), lightCol);          // left
                dl->AddLine(ImVec2(a.x + o, b.y - 1 - o), ImVec2(b.x - o, b.y - 1 - o), darkCol);   // bottom
                dl->AddLine(ImVec2(b.x - 1 - o, a.y + o), ImVec2(b.x - 1 - o, b.y - o), darkCol);   // right
            }
        }

        // Etched line frame (group boxes): dark groove with a light ridge inside.
        void Etch(ImDrawList* dl, ImVec2 a, ImVec2 b) {
            dl->AddRect(a, ImVec2(b.x - 1, b.y - 1), BEVEL_DARK);
            dl->AddRect(ImVec2(a.x + 1, a.y + 1), b, BEVEL_LIGHT);
        }

        ImU32 SegmentColor(float t, bool lit) {
            ImU32 c = t < 0.6f ? IM_COL32(0x00, 0xE8, 0x3C, 0xFF) : t < 0.85f ? IM_COL32(0xF4, 0xD4, 0x00, 0xFF) : IM_COL32(0xFF, 0x34, 0x20, 0xFF);
            if (!lit) {
                const int r = int((c >> IM_COL32_R_SHIFT) & 0xFF) / 6, g = int((c >> IM_COL32_G_SHIFT) & 0xFF) / 6, b = int((c >> IM_COL32_B_SHIFT) & 0xFF) / 6;
                c = IM_COL32(r, g, b, 0xFF);
            }
            return c;
        }

        // Gradient green -> amber -> red at t in [0, 1].
        ImU32 EqColor(float t) {
            t = std::clamp(t, 0.0f, 1.0f);
            const ImVec4 green(0.0f, 0.80f, 0.22f, 1.0f), amber(1.0f, 0.70f, 0.0f, 1.0f), red(0.95f, 0.16f, 0.10f, 1.0f);
            const ImVec4 a = t < 0.5f ? green : amber, b = t < 0.5f ? amber : red;
            const float u = t < 0.5f ? t * 2.0f : (t - 0.5f) * 2.0f;
            return ImGui::GetColorU32(ImVec4(a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u, 1.0f));
        }

        void GlowText(ImDrawList* dl, float size, ImVec2 pos, ImU32 color, ImU32 glow, const char* text) {
            ImFont* font = ImGui::GetFont();
            for (const ImVec2 o : { ImVec2(-1, 0), ImVec2(1, 0), ImVec2(0, -1), ImVec2(0, 1) }) dl->AddText(font, size, Add(pos, o), glow, text);
            dl->AddText(font, size, pos, color, text);
        }

        // Small recessed LCD field with a value (sliders, meters, readouts).
        void LcdBox(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* text, bool amber) {
            dl->AddRectFilled(a, b, BLACK);
            Bevel(dl, a, b, true, 1);
            const ImVec2 ts = ImGui::CalcTextSize(text);
            const ImVec2 pos(b.x - ts.x - 4.0f, (a.y + b.y - ts.y) * 0.5f);
            dl->PushClipRect(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 1), true);
            GlowText(dl, ImGui::GetFontSize(), pos, amber ? AMBER_COL : GREEN, amber ? IM_COL32(0xFF, 0xBE, 0x0B, 0x20) : IM_COL32(0x00, 0xFF, 0x41, 0x20), text);
            dl->PopClipRect();
        }

        void Led(ImDrawList* dl, ImVec2 center, int state) {
            const ImU32 on = state == 2 ? AMBER_COL : GREEN;
            const ImU32 off = IM_COL32(0x2A, 0x3A, 0x2A, 0xFF);
            if (state > 0) dl->AddCircleFilled(center, 5.5f, state == 2 ? AMBER_GLOW : GREEN_GLOW);
            dl->AddCircleFilled(center, 3.5f, state > 0 ? on : off);
            dl->AddCircle(center, 3.5f, BEVEL_DARK);
            if (state > 0) dl->AddCircleFilled(Add(center, ImVec2(-1.0f, -1.0f)), 1.2f, IM_COL32(0xFF, 0xFF, 0xFF, 0xB0));
        }

        // Tactile button face: dark gunmetal gradient, two-pixel bevel (inverted while pressed).
        void Face(ImDrawList* dl, ImVec2 a, ImVec2 b, bool pressed, bool hovered) {
            const ImU32 top = pressed ? IM_COL32(0x15, 0x16, 0x1D, 0xFF) : hovered ? IM_COL32(0x34, 0x39, 0x49, 0xFF) : IM_COL32(0x2C, 0x30, 0x3D, 0xFF);
            const ImU32 bottom = pressed ? IM_COL32(0x1F, 0x21, 0x2B, 0xFF) : IM_COL32(0x1B, 0x1D, 0x26, 0xFF);
            dl->AddRectFilledMultiColor(a, b, top, top, bottom, bottom);
            dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), BEVEL_DARK);
            Bevel(dl, a, b, pressed, 2);
        }

        float Fraction(float v, float lo, float hi) { return hi > lo ? std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f; }

        struct GroupFrame { ImVec2 start; float width; std::string title; ImDrawList* drawList; };
        std::vector<GroupFrame> g_groups;
        float g_menuBarHeight = 0.0f;
    }

    // ---------------------------------------------------------------------------------- theme
    void ApplyTheme() {
        ImGui::StyleColorsDark();
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding = 0.0f;
        s.ChildRounding = 0.0f;
        s.FrameRounding = 0.0f;
        s.PopupRounding = 0.0f;
        s.ScrollbarRounding = 0.0f;
        s.GrabRounding = 0.0f;
        s.TabRounding = 0.0f;
        s.WindowBorderSize = 0.0f;        // the bevel is drawn by BeginPanel
        s.FrameBorderSize = 1.0f;         // Border + BorderShadow give every frame a 1 px bevel
        s.ChildBorderSize = 1.0f;
        s.PopupBorderSize = 1.0f;
        s.WindowPadding = ImVec2(9, 7);
        s.FramePadding = ImVec2(6, 3);
        s.ItemSpacing = ImVec2(6, 5);
        s.GrabMinSize = 12.0f;
        s.ScrollbarSize = 14.0f;
        s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
        s.WindowMenuButtonPosition = ImGuiDir_None;
        s.SeparatorTextBorderSize = 2.0f;
        ImVec4* c = s.Colors;
        c[ImGuiCol_Text] = V(0xE2, 0xE6, 0xF0);
        c[ImGuiCol_TextDisabled] = V(0x8A, 0x90, 0xA2);
        c[ImGuiCol_WindowBg] = V(0x1B, 0x1D, 0x26, 0xF8);
        c[ImGuiCol_ChildBg] = V(0, 0, 0, 0);
        c[ImGuiCol_PopupBg] = V(0x12, 0x13, 0x1A, 0xFC);
        c[ImGuiCol_Border] = V(0x3E, 0x43, 0x56);
        c[ImGuiCol_BorderShadow] = V(0x07, 0x08, 0x0B);
        c[ImGuiCol_FrameBg] = V(0x12, 0x13, 0x1A);
        c[ImGuiCol_FrameBgHovered] = V(0x25, 0x28, 0x34);
        c[ImGuiCol_FrameBgActive] = V(0x2C, 0x30, 0x3D);
        c[ImGuiCol_TitleBg] = V(0x12, 0x13, 0x1A);
        c[ImGuiCol_TitleBgActive] = V(0x25, 0x28, 0x34);
        c[ImGuiCol_TitleBgCollapsed] = V(0x12, 0x13, 0x1A);
        c[ImGuiCol_MenuBarBg] = V(0x25, 0x28, 0x34);
        c[ImGuiCol_ScrollbarBg] = V(0x12, 0x13, 0x1A);
        c[ImGuiCol_ScrollbarGrab] = V(0x3E, 0x43, 0x56);
        c[ImGuiCol_ScrollbarGrabHovered] = V(0x5A, 0x60, 0x74);
        c[ImGuiCol_ScrollbarGrabActive] = V(0x6E, 0x75, 0x90);
        c[ImGuiCol_CheckMark] = V(0xFF, 0xBE, 0x0B);
        c[ImGuiCol_SliderGrab] = V(0x5A, 0x60, 0x74);
        c[ImGuiCol_SliderGrabActive] = V(0xFF, 0xBE, 0x0B);
        c[ImGuiCol_Button] = V(0x25, 0x28, 0x34);
        c[ImGuiCol_ButtonHovered] = V(0x34, 0x39, 0x49);
        c[ImGuiCol_ButtonActive] = V(0x15, 0x16, 0x1D);
        c[ImGuiCol_Header] = V(0x2C, 0x30, 0x3D);
        c[ImGuiCol_HeaderHovered] = V(0x3E, 0x43, 0x56);
        c[ImGuiCol_HeaderActive] = V(0x4A, 0x50, 0x66);
        c[ImGuiCol_Separator] = V(0x3E, 0x43, 0x56);
        c[ImGuiCol_SeparatorHovered] = V(0xFF, 0xBE, 0x0B);
        c[ImGuiCol_SeparatorActive] = V(0xFF, 0xBE, 0x0B);
        c[ImGuiCol_ResizeGrip] = V(0x3E, 0x43, 0x56, 0x80);
        c[ImGuiCol_ResizeGripHovered] = V(0xFF, 0xBE, 0x0B, 0xA0);
        c[ImGuiCol_ResizeGripActive] = V(0xFF, 0xBE, 0x0B);
        c[ImGuiCol_Tab] = V(0x12, 0x13, 0x1A);
        c[ImGuiCol_TabHovered] = V(0x34, 0x39, 0x49);
        c[ImGuiCol_TabSelected] = V(0x25, 0x28, 0x34);
        c[ImGuiCol_PlotLines] = V(0x00, 0xFF, 0x41);
        c[ImGuiCol_PlotLinesHovered] = V(0xFF, 0xBE, 0x0B);
        c[ImGuiCol_PlotHistogram] = V(0xFF, 0xBE, 0x0B);
        c[ImGuiCol_PlotHistogramHovered] = V(0xFF, 0xD8, 0x60);
        c[ImGuiCol_TextSelectedBg] = V(0xFF, 0xBE, 0x0B, 0x50);
        c[ImGuiCol_NavHighlight] = V(0xFF, 0xBE, 0x0B);
    }

    // ---------------------------------------------------------------------------------- panel
    namespace {
        bool PanelFrame(const char* title, bool visible);
    }

    bool BeginPanel(const char* title, float x, float y, float width, float height, bool* open) {
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_FirstUseEver);
        return PanelFrame(title, ImGui::Begin(title, open));
    }

    bool BeginDockedPanel(const char* title, float x, float y, float width, float height, int flags) {
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
        return PanelFrame(title, ImGui::Begin(title, nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                                              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | flags));
    }

    namespace {
    bool PanelFrame(const char* title, bool visible) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        const ImVec2 q(p.x + size.x, p.y + size.y);
        const float th = ImGui::GetFrameHeight();
        dl->PushClipRect(p, q, false);
        // Satin console finish: faint horizontal streaks over the grey body.
        if (visible) {
            for (float yy = p.y + th + 1.0f; yy < q.y - 2.0f; yy += 2.0f) {
                const int phase = int((yy - p.y) * 0.5f) % 9;
                dl->AddLine(ImVec2(p.x + 2, yy), ImVec2(q.x - 2, yy), phase < 4 ? IM_COL32(0xFF, 0xFF, 0xFF, 0x04) : IM_COL32(0, 0, 0, 0x10));
            }
        }
        // Title bar: slate-blue gradient, light double stripes either side of the centred title.
        const ImVec2 ta(p.x, p.y), tb(q.x, p.y + th);
        dl->AddRectFilledMultiColor(ta, tb, TITLE_TOP, TITLE_TOP, TITLE_BOT, TITLE_BOT);
        const char* end = std::strstr(title, "##");
        const ImVec2 textSize = ImGui::CalcTextSize(title, end);
        const float cx = p.x + size.x * 0.5f, cy = p.y + th * 0.5f;
        const float textLeft = cx - textSize.x * 0.5f - 8.0f, textRight = cx + textSize.x * 0.5f + 8.0f;
        for (const float dy : { -2.0f, 2.0f }) {
            if (textLeft > p.x + 10.0f) dl->AddLine(ImVec2(p.x + 6.0f, cy + dy), ImVec2(textLeft, cy + dy), dy < 0 ? STRIPE : STRIPE_DIM, 1.5f);
            if (textRight < q.x - 10.0f) dl->AddLine(ImVec2(textRight, cy + dy), ImVec2(q.x - 6.0f, cy + dy), dy < 0 ? STRIPE : STRIPE_DIM, 1.5f);
        }
        dl->AddText(ImVec2(cx - textSize.x * 0.5f + 1.0f, cy - textSize.y * 0.5f + 1.0f), IM_COL32(0x08, 0x12, 0x24, 0xFF), title, end);
        dl->AddText(ImVec2(cx - textSize.x * 0.5f, cy - textSize.y * 0.5f), INK_COL, title, end);
        Bevel(dl, ta, tb, false, 1);
        // Chiseled outer frame: two-pixel raised bevel plus a dark outline.
        dl->AddRect(p, q, BEVEL_DARK);
        Bevel(dl, ImVec2(p.x + 1, p.y + 1), ImVec2(q.x - 1, q.y - 1), false, 2);
        dl->PopClipRect();
        if (g_current.panels < 8) {
            float* r = g_current.panelRects[g_current.panels];
            r[0] = p.x; r[1] = p.y; r[2] = size.x; r[3] = size.y;
        }
        ++g_current.panels;
        return visible;
    }
    }

    void EndPanel() { ImGui::End(); }

    // ---------------------------------------------------------------------------------- menu bar
    bool BeginMenuBar(const char* title) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 5));
        const bool open = ImGui::BeginMainMenuBar();
        ImGui::PopStyleVar();
        if (!open) return false;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        g_menuBarHeight = size.y;
        dl->PushClipRect(p, ImVec2(p.x + size.x, p.y + size.y), false);
        dl->AddRectFilledMultiColor(p, ImVec2(p.x + size.x, p.y + size.y), TITLE_TOP, TITLE_TOP, TITLE_BOT, TITLE_BOT);
        Bevel(dl, p, ImVec2(p.x + size.x, p.y + size.y), false, 2);
        dl->PopClipRect();
        if (title && *title) {
            // engraved title plate on the left: slate-blue, white letters
            const ImVec2 ts = ImGui::CalcTextSize(title);
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const ImVec2 pa(a.x, p.y + 3.0f), pb(a.x + ts.x + 16.0f, p.y + size.y - 3.0f);
            dl->AddRectFilledMultiColor(pa, pb, TITLE_TOP, TITLE_TOP, TITLE_BOT, TITLE_BOT);
            Bevel(dl, pa, pb, true, 1);
            dl->AddText(ImVec2(pa.x + 8.0f, (pa.y + pb.y - ts.y) * 0.5f), HEADING_COL, title);
            ImGui::Dummy(ImVec2(ts.x + 20.0f, ts.y));
        }
        ++g_current.menuBars;
        return true;
    }

    void EndMenuBar() { ImGui::EndMainMenuBar(); }
    float MenuBarHeight() { return g_menuBarHeight; }

    // ---------------------------------------------------------------------------------- group box
    void BeginGroup(const char* title, float width) {
        const float w = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
        ImGui::Dummy(ImVec2(w, 3.0f));
        GroupFrame f{ ImGui::GetCursorScreenPos(), w, title ? title : "", ImGui::GetWindowDrawList() };
        g_groups.push_back(f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, ImGui::GetTextLineHeight() * 0.5f + 6.0f));
        ImGui::BeginChild(title, ImVec2(w, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar();
    }

    void EndGroup() {
        ImGui::EndChild();
        if (g_groups.empty()) return;
        const GroupFrame f = g_groups.back();
        g_groups.pop_back();
        const ImVec2 b = ImGui::GetItemRectMax();
        ImDrawList* dl = f.drawList;
        const float half = ImGui::GetTextLineHeight() * 0.5f;
        const ImVec2 a(f.start.x, f.start.y + half);
        Etch(dl, a, ImVec2(f.start.x + f.width, b.y));
        if (!f.title.empty()) {
            const char* text = f.title.c_str();
            const char* end = std::strstr(text, "##");
            const ImVec2 ts = ImGui::CalcTextSize(text, end);
            const ImVec2 ta(f.start.x + 8.0f, f.start.y);
            dl->AddRectFilled(ImVec2(ta.x - 3.0f, ta.y), ImVec2(ta.x + ts.x + 3.0f, ta.y + ts.y), PANEL);
            dl->AddText(ta, HEADING_COL, text, end);
        }
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ++g_current.groups;
    }

    // ---------------------------------------------------------------------------------- LCD
    void Lcd(const char* bigText, const char* smallText, float height, bool amber) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float w = std::max(ImGui::GetContentRegionAvail().x, 60.0f);
        const float bigSize = ImGui::GetFontSize() * 2.4f, smallSize = ImGui::GetFontSize();
        const float h = height > 0.0f ? height : bigSize + (smallText ? smallSize + 10.0f : 10.0f);
        const ImVec2 b(a.x + w, a.y + h);
        const ImU32 lit = amber ? AMBER_COL : GREEN, glow = amber ? AMBER_GLOW : GREEN_GLOW, ghostCol = amber ? AMBER_GHOST : GREEN_GHOST;
        dl->AddRectFilled(a, b, BLACK);
        Bevel(dl, a, b, true, 2);
        // Faint scanlines.
        for (float y = a.y + 3.0f; y < b.y - 2.0f; y += 3.0f) dl->AddLine(ImVec2(a.x + 2, y), ImVec2(b.x - 2, y), amber ? IM_COL32(0x40, 0x2C, 0x00, 0x18) : IM_COL32(0x00, 0x40, 0x10, 0x18));
        // Ghost segments: every digit position shows a dim "8" behind the lit glyph.
        std::string ghost = bigText ? bigText : "";
        for (char& ch : ghost) if (ch >= '0' && ch <= '9') ch = '8';
        const ImVec2 pos(a.x + 8.0f, a.y + 4.0f);
        dl->AddText(ImGui::GetFont(), bigSize, pos, ghostCol, ghost.c_str());
        GlowText(dl, bigSize, pos, lit, glow, bigText ? bigText : "");
        if (smallText) GlowText(dl, smallSize, ImVec2(a.x + 9.0f, a.y + 6.0f + bigSize), lit, amber ? IM_COL32(0xFF, 0xBE, 0x0B, 0x20) : IM_COL32(0x00, 0xFF, 0x41, 0x20), smallText);
        ImGui::Dummy(ImVec2(w, h));
        g_current.lcdRect[0] = a.x; g_current.lcdRect[1] = a.y; g_current.lcdRect[2] = w; g_current.lcdRect[3] = h;
        ++g_current.lcds;
    }

    void LcdField(const char* label, const char* value, bool amber, float valueWidth) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        const float h = ImGui::GetTextLineHeight() + 6.0f;
        const float vw = valueWidth > 0.0f ? valueWidth : std::max(avail * 0.55f, 80.0f);
        dl->AddText(ImVec2(start.x, start.y + 3.0f), INK_COL, label);
        LcdBox(dl, ImVec2(start.x + avail - vw, start.y), ImVec2(start.x + avail, start.y + h), value, amber);
        ImGui::Dummy(ImVec2(avail, h));
        ++g_current.lcdFields;
    }

    // ---------------------------------------------------------------------------------- meters
    void LedMeter(const char* label, float fraction, const char* valueText) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        const float labelW = 46.0f, valueW = valueText ? 96.0f : 0.0f;
        const float lineH = ImGui::GetTextLineHeight();
        const float h = std::max(lineH + 4.0f, 14.0f);
        dl->AddText(ImVec2(start.x, start.y + (h - lineH) * 0.5f), INK_COL, label);
        const ImVec2 a(start.x + labelW, start.y), b(start.x + std::max(avail - valueW - 4.0f, labelW + 40.0f), start.y + h);
        dl->AddRectFilled(a, b, BLACK);
        Bevel(dl, a, b, true, 1);
        const int n = std::clamp(int((b.x - a.x - 4.0f) / 6.0f), 8, 48);
        const float segW = (b.x - a.x - 4.0f) / float(n);
        const int lit = int(std::round(std::clamp(fraction, 0.0f, 1.0f) * float(n)));
        for (int i = 0; i < n; ++i) {
            const float x0 = a.x + 2.0f + segW * i;
            dl->AddRectFilled(ImVec2(x0, a.y + 2.0f), ImVec2(x0 + segW - 1.5f, b.y - 2.0f), SegmentColor((i + 0.5f) / float(n), i < lit));
        }
        if (valueText) LcdBox(dl, ImVec2(b.x + 4.0f, start.y), ImVec2(start.x + avail, start.y + h), valueText, false);
        ImGui::Dummy(ImVec2(avail, h));
        ++g_current.meters;
        g_current.litSegments += lit;
    }

    void Analyzer(const char* id, const float* values, int count, int offset, float height) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 b(a.x + w, a.y + height);
        dl->AddRectFilled(a, b, BLACK);
        Bevel(dl, a, b, true, 1);
        const int columns = std::clamp(int((w - 4.0f) / 5.0f), 8, 96);
        const float colW = (w - 4.0f) / float(columns);
        const int rows = std::max(int((height - 4.0f) / 3.0f), 4);
        for (int c = 0; c < columns && count > 0; ++c) {
            const int index = ((offset - columns + c) % count + count) % count;
            const float v = std::clamp(values[index], 0.0f, 1.0f);
            const int litRows = int(std::ceil(v * rows));
            for (int r = 0; r < litRows; ++r) {
                const float y1 = b.y - 2.0f - r * 3.0f;
                dl->AddRectFilled(ImVec2(a.x + 2.0f + c * colW, y1 - 2.0f), ImVec2(a.x + 2.0f + (c + 1) * colW - 1.0f, y1), SegmentColor((r + 0.5f) / rows, true));
            }
        }
        ImGui::PushID(id);
        ImGui::Dummy(ImVec2(w, height));
        ImGui::PopID();
    }

    // ---------------------------------------------------------------------------------- sliders
    namespace {
        bool EqBehaviour(const char* label, float& t, bool vertical, ImVec2 size, ImVec2& a, ImVec2& b, bool& active, bool& hovered) {
            a = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton(label, size);
            b = ImVec2(a.x + size.x, a.y + size.y);
            active = ImGui::IsItemActive();
            hovered = ImGui::IsItemHovered();
            const float before = t;
            if (active) {
                const ImVec2 m = ImGui::GetIO().MousePos;
                t = vertical ? 1.0f - (m.y - a.y - 5.0f) / std::max(size.y - 10.0f, 1.0f) : (m.x - a.x - 6.0f) / std::max(size.x - 12.0f, 1.0f);
                t = std::clamp(t, 0.0f, 1.0f);
            }
            else if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
                t = std::clamp(t + ImGui::GetIO().MouseWheel * 0.02f, 0.0f, 1.0f);
            }
            return t != before;
        }

        void DrawEq(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, bool vertical, bool active, bool hovered) {
            const ImU32 c0 = EqColor(0.0f), c1 = EqColor(0.5f), c2 = EqColor(1.0f);
            if (vertical) {
                const float cx = (a.x + b.x) * 0.5f;
                const ImVec2 ta(cx - 3.0f, a.y + 4.0f), tb(cx + 3.0f, b.y - 4.0f);
                const float mid = (ta.y + tb.y) * 0.5f;
                dl->AddRectFilledMultiColor(ImVec2(ta.x, ta.y), ImVec2(tb.x, mid), c2, c2, c1, c1);
                dl->AddRectFilledMultiColor(ImVec2(ta.x, mid), ImVec2(tb.x, tb.y), c1, c1, c0, c0);
                const float y = tb.y - t * (tb.y - ta.y);
                dl->AddRectFilled(ta, ImVec2(tb.x, y), IM_COL32(0x07, 0x08, 0x0B, 0xE8));   // unlit part above the thumb
                Bevel(dl, ImVec2(ta.x - 1, ta.y - 1), ImVec2(tb.x + 1, tb.y + 1), true, 1);
                const ImVec2 ka(a.x + 1.0f, y - 5.0f), kb(b.x - 1.0f, y + 5.0f);
                dl->AddRectFilledMultiColor(ka, kb, active ? METAL_ACTIVE : METAL_HI, active ? METAL_ACTIVE : METAL_HI, METAL_LO, METAL_LO);
                dl->AddRect(ImVec2(ka.x - 1, ka.y - 1), ImVec2(kb.x + 1, kb.y + 1), BEVEL_DARK);
                Bevel(dl, ka, kb, false, 1);
                dl->AddLine(ImVec2(ka.x + 3, y), ImVec2(kb.x - 3, y), hovered || active ? ACCENT : BEVEL_DARK);
            }
            else {
                const float cy = (a.y + b.y) * 0.5f;
                const ImVec2 ta(a.x + 6.0f, cy - 3.0f), tb(b.x - 6.0f, cy + 3.0f);
                const float mid = (ta.x + tb.x) * 0.5f;
                dl->AddRectFilledMultiColor(ImVec2(ta.x, ta.y), ImVec2(mid, tb.y), c0, c1, c1, c0);
                dl->AddRectFilledMultiColor(ImVec2(mid, ta.y), ImVec2(tb.x, tb.y), c1, c2, c2, c1);
                const float x = ta.x + t * (tb.x - ta.x);
                dl->AddRectFilled(ImVec2(x, ta.y), tb, IM_COL32(0x07, 0x08, 0x0B, 0xE8));
                Bevel(dl, ImVec2(ta.x - 1, ta.y - 1), ImVec2(tb.x + 1, tb.y + 1), true, 1);
                const ImVec2 ka(x - 5.0f, a.y + 1.0f), kb(x + 5.0f, b.y - 1.0f);
                dl->AddRectFilledMultiColor(ka, kb, active ? METAL_ACTIVE : METAL_HI, active ? METAL_ACTIVE : METAL_HI, METAL_LO, METAL_LO);
                dl->AddRect(ImVec2(ka.x - 1, ka.y - 1), ImVec2(kb.x + 1, kb.y + 1), BEVEL_DARK);
                Bevel(dl, ka, kb, false, 1);
                dl->AddLine(ImVec2(x, ka.y + 3), ImVec2(x, kb.y - 3), hovered || active ? ACCENT : BEVEL_DARK);
            }
        }

        bool EqCommon(const char* label, float& t, const char* valueText, bool vertical, float length) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const char* end = std::strstr(label, "##");
            const std::string visible(label, end ? end : label + std::strlen(label));
            bool changed = false, active = false, hovered = false;
            ImVec2 a, b;
            ImGui::PushID(label);
            if (vertical) {
                const float len = length > 0.0f ? length : 104.0f;
                const float colW = std::max({ ImGui::CalcTextSize(visible.c_str()).x, ImGui::CalcTextSize(valueText).x + 8.0f, 34.0f }) + 4.0f;
                ImGui::BeginGroup();
                const ImVec2 top = ImGui::GetCursorScreenPos();
                const float lh = ImGui::GetTextLineHeight() + 4.0f;
                LcdBox(dl, top, ImVec2(top.x + colW, top.y + lh), valueText, false);
                ImGui::Dummy(ImVec2(colW, lh));
                ImGui::SetCursorScreenPos(ImVec2(top.x + (colW - 22.0f) * 0.5f, ImGui::GetCursorScreenPos().y));
                changed = EqBehaviour("##eq", t, true, ImVec2(22.0f, len), a, b, active, hovered);
                DrawEq(dl, a, b, t, true, active, hovered);
                const ImVec2 ls = ImGui::CalcTextSize(visible.c_str());
                const ImVec2 lp = ImGui::GetCursorScreenPos();
                dl->AddText(ImVec2(top.x + (colW - ls.x) * 0.5f, lp.y), HEADING_COL, visible.c_str());
                ImGui::SetCursorScreenPos(ImVec2(top.x, lp.y));
                ImGui::Dummy(ImVec2(colW, ImGui::GetTextLineHeight()));
                ImGui::EndGroup();
            }
            else {
                const float labelW = 112.0f, valueW = 72.0f;
                const float avail = ImGui::GetContentRegionAvail().x;
                const ImVec2 start = ImGui::GetCursorScreenPos();
                const float h = 18.0f;
                dl->AddText(ImVec2(start.x, start.y + (h - ImGui::GetTextLineHeight()) * 0.5f), INK_COL, visible.c_str());
                ImGui::SetCursorScreenPos(ImVec2(start.x + labelW, start.y));
                const float trackW = length > 0.0f ? length : std::max(avail - labelW - valueW, 60.0f);
                changed = EqBehaviour("##eq", t, false, ImVec2(trackW, h), a, b, active, hovered);
                DrawEq(dl, a, b, t, false, active, hovered);
                LcdBox(dl, ImVec2(b.x + 4.0f, start.y), ImVec2(std::max(start.x + avail, b.x + 50.0f), start.y + h), valueText, false);
                ImGui::SetCursorScreenPos(ImVec2(start.x, start.y));
                ImGui::Dummy(ImVec2(avail, h));
            }
            if (hovered && !active) ImGui::SetTooltip("%s: %s (drag, or mouse wheel)", visible.c_str(), valueText);
            ImGui::PopID();
            ++g_current.eqSliders;
            return changed;
        }
    }

    bool EqSlider(const char* label, float* value, float minValue, float maxValue, const char* format, bool vertical, float length) {
        float t = Fraction(*value, minValue, maxValue);
        char text[48];
        std::snprintf(text, sizeof(text), format ? format : "%.2f", double(*value));
        if (!EqCommon(label, t, text, vertical, length)) return false;
        const float v = minValue + t * (maxValue - minValue);
        if (v == *value) return false;
        *value = v;
        return true;
    }

    bool EqSliderInt(const char* label, int* value, int minValue, int maxValue, bool vertical, float length) {
        float t = Fraction(float(*value), float(minValue), float(maxValue));
        char text[24];
        std::snprintf(text, sizeof(text), "%d", *value);
        if (!EqCommon(label, t, text, vertical, length)) return false;
        const int v = int(std::lround(float(minValue) + t * float(maxValue - minValue)));
        if (v == *value) return false;
        *value = v;
        return true;
    }

    // ---------------------------------------------------------------------------------- buttons
    bool TransportButton(const char* id, Glyph glyph, bool lit, const char* tooltip) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const ImVec2 size(30.0f, 22.0f);
        const bool clicked = ImGui::InvisibleButton(id, size);
        const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
        const ImVec2 b(a.x + size.x, a.y + size.y);
        Face(dl, a, b, active, hovered);
        const ImU32 col = lit ? GREEN : INK_COL;
        const float off = active ? 1.0f : 0.0f;
        const ImVec2 c((a.x + b.x) * 0.5f + off, (a.y + b.y) * 0.5f + off);
        switch (glyph) {
            case Glyph::Play:
                dl->AddTriangleFilled(ImVec2(c.x - 4, c.y - 6), ImVec2(c.x - 4, c.y + 6), ImVec2(c.x + 6, c.y), col);
                break;
            case Glyph::Pause:
                dl->AddRectFilled(ImVec2(c.x - 6, c.y - 6), ImVec2(c.x - 2, c.y + 6), col);
                dl->AddRectFilled(ImVec2(c.x + 2, c.y - 6), ImVec2(c.x + 6, c.y + 6), col);
                break;
            case Glyph::Stop:
                dl->AddRectFilled(ImVec2(c.x - 5, c.y - 5), ImVec2(c.x + 5, c.y + 5), col);
                break;
            case Glyph::StepBack:
                dl->AddRectFilled(ImVec2(c.x - 7, c.y - 6), ImVec2(c.x - 5, c.y + 6), col);
                dl->AddTriangleFilled(ImVec2(c.x + 5, c.y - 6), ImVec2(c.x + 5, c.y + 6), ImVec2(c.x - 4, c.y), col);
                break;
            case Glyph::StepForward:
                dl->AddTriangleFilled(ImVec2(c.x - 5, c.y - 6), ImVec2(c.x - 5, c.y + 6), ImVec2(c.x + 4, c.y), col);
                dl->AddRectFilled(ImVec2(c.x + 5, c.y - 6), ImVec2(c.x + 7, c.y + 6), col);
                break;
            case Glyph::Record:
                dl->AddCircleFilled(c, 5.0f, lit ? IM_COL32(0xE8, 0x20, 0x10, 0xFF) : IM_COL32(0x90, 0x30, 0x28, 0xFF));
                if (lit) dl->AddCircle(c, 7.0f, IM_COL32(0xE8, 0x20, 0x10, 0x90));
                break;
            case Glyph::Eject:
                dl->AddTriangleFilled(ImVec2(c.x - 6, c.y + 1), ImVec2(c.x + 6, c.y + 1), ImVec2(c.x, c.y - 6), col);
                dl->AddRectFilled(ImVec2(c.x - 6, c.y + 3), ImVec2(c.x + 6, c.y + 6), col);
                break;
        }
        if (tooltip && hovered) ImGui::SetTooltip("%s", tooltip);
        ++g_current.transportButtons;
        return clicked;
    }

    bool Button(const char* label, float width, int led) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const char* end = std::strstr(label, "##");
        const ImVec2 ts = ImGui::CalcTextSize(label, end);
        const float ledW = led >= 0 ? 14.0f : 0.0f;
        const float w = width > 0.0f ? width : width < 0.0f ? ImGui::GetContentRegionAvail().x : ts.x + 16.0f + ledW;
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const ImVec2 size(w, ImGui::GetFrameHeight() + 2.0f);
        const bool clicked = ImGui::InvisibleButton(label, size);
        const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
        const ImVec2 b(a.x + size.x, a.y + size.y);
        Face(dl, a, b, active, hovered);
        const float off = active ? 1.0f : 0.0f;
        if (led >= 0) Led(dl, ImVec2(a.x + 9.0f + off, (a.y + b.y) * 0.5f + off), led);
        const float tx = a.x + ledW + (w - ledW - ts.x) * 0.5f + off, ty = (a.y + b.y - ts.y) * 0.5f + off;
        dl->AddText(ImVec2(tx + 1, ty + 1), BEVEL_DARK, label, end);
        dl->AddText(ImVec2(tx, ty), led > 0 ? HEADING_COL : INK_COL, label, end);
        return clicked;
    }

    bool LedToggle(const char* label, bool* value) {
        if (!Button(label, 0.0f, *value ? 1 : 0)) return false;
        *value = !*value;
        return true;
    }

    // ---------------------------------------------------------------------------------- playlist
    bool BeginPlaylist(const char* id, float height) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 1));
        ImGui::PushStyleColor(ImGuiCol_Border, V(0x07, 0x08, 0x0B));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 3));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));
        const bool open = ImGui::BeginChild(id, ImVec2(0.0f, height), ImGuiChildFlags_Borders);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
        dl->PushClipRect(p, ImVec2(p.x + s.x, p.y + s.y), false);
        Bevel(dl, p, ImVec2(p.x + s.x, p.y + s.y), true, 2);
        dl->PopClipRect();
        return open;
    }

    bool PlaylistRow(int number, const char* text, const char* rightText, bool selected) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = ImGui::GetTextLineHeight() + 3.0f;
        ImGui::PushID(number);
        const bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h));
        ImGui::PopID();
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 b(a.x + w, a.y + h);
        if (selected) {
            dl->AddRectFilled(a, b, SELECT_BG);
            dl->AddRect(a, b, AMBER_COL);
        }
        else if (hovered) dl->AddRectFilled(a, b, HOVER_BG);
        char prefix[16];
        std::snprintf(prefix, sizeof(prefix), "%d.", number);
        const float ty = a.y + 1.5f;
        dl->AddText(ImVec2(a.x + 3.0f, ty), selected ? AMBER_COL : GREEN, prefix);
        dl->AddText(ImVec2(a.x + 26.0f, ty), selected ? WHITE : GREEN, text);
        if (rightText && *rightText) {
            const ImVec2 rs = ImGui::CalcTextSize(rightText);
            dl->AddText(ImVec2(b.x - rs.x - 4.0f, ty), selected ? AMBER_COL : GREEN, rightText);
        }
        ++g_current.playlistRows;
        return clicked;
    }

    void EndPlaylist() {
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    void PushPlaylistTreeStyle() {
        ImGui::PushStyleColor(ImGuiCol_Text, V(0x00, 0xFF, 0x41));
        ImGui::PushStyleColor(ImGuiCol_Header, V(0x20, 0x3C, 0x78));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, V(0x0C, 0x2C, 0x16));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, V(0x30, 0x50, 0x90));
    }
    void PopPlaylistTreeStyle() { ImGui::PopStyleColor(4); }

    const FrameStats& LastFrameStats() { return g_last; }
    void ResetFrameStats() { g_last = g_current; g_current = FrameStats{}; g_groups.clear(); }
}
