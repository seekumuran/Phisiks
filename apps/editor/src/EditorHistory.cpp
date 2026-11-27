// Undo / redo and edit / play mode.
//
// History. The editor keeps a baseline: the authored state of every entity (SceneSerializer entity
// JSON, without children, plus the parent's guid), keyed by EntityGuid. When an edit was noted
// (NoteEdit / MarkEdited / EndEdit) and no interaction is in progress (no active widget, no gizmo
// drag, no drag-and-drop), the current state is diffed against the baseline: the changed, created
// and deleted entities become one command (before / after state per entity), and the baseline moves
// on. A continuous interaction (dragging a value or a gizmo, typing a name) is therefore one step.
// The baseline is refreshed whenever an interaction begins, so changes made by scene logic in
// between are not attributed to the user; the selected entity's state is also kept from the start
// of each frame, for widgets that change a value in the frame they activate (sliders).
// The authored document is what File > Save writes: the workspace persistence hooks run around
// every capture (an exploded CAD view is captured assembled).
//
// Undo / redo apply a command's before / after states through SceneSerializer (create missing
// entities with their guids, apply state, set parents, destroy entities that must not exist), so
// entity references by guid stay valid across any number of cycles.
//
// Play mode. EnterPlay captures the authored document (ToJson) and runs the simulation; StopPlay
// restores it in place (RestoreSnapshot: same handles for surviving entities) and calls the
// workspace's OnAfterLoad. Nothing done while playing enters the history, and saving is refused.
#include "Editor.h"
#include "EditorInternal.h"

#include "Phisiks/Core/Log.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Physics/Advanced.h"
#include "Phisiks/Scene/SceneSerializer.h"
#include "Phisiks/UI/ImGuiLayer.h"

#include <imgui.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>

using namespace Phisiks;

namespace Editor {
    namespace {
        struct EntityState {
            std::string json;          // SceneSerializer::EntityToJson(e, false)
            uint64_t parent = 0;       // parent guid, 0 = root
            bool operator==(const EntityState& o) const { return json == o.json && parent == o.parent; }
        };
        struct Change {
            uint64_t guid = 0;
            std::optional<EntityState> before, after;
        };
        struct Command {
            std::string label;
            std::vector<Change> changes;
            size_t bytes = 0;
        };
        using Baseline = std::unordered_map<uint64_t, EntityState>;

        struct State {
            uint64_t sceneSerial = 0;
            Baseline baseline;
            bool baselineValid = false;
            uint64_t editSerial = 0, committedSerial = 0;
            bool interacting = false;               // previous frame
            uint64_t selectedGuid = 0;              // selection at the start of the frame
            std::optional<EntityState> selectedPre;  // its state at the start of the interaction / frame
            std::deque<Command> undo, redo;
            size_t bytes = 0;
            size_t maxEntries = 256, maxBytes = size_t(64) << 20;
            std::string explicitLabel;              // BeginEdit label
            bool explicitOpen = false;
            double lastCaptureMs = 0.0;
            // play mode
            Mode mode = Mode::Edit;
            std::string snapshot;
            uint64_t playSelection = 0;
            bool hookInstalled = false;
        };
        State g;

        Scene* CurrentScene(GameContext& ctx) { return ctx.CurrentScene(); }

        std::optional<EntityState> StateOf(Entity e) {
            if (!e || Internal::IsEditorEntity(e)) return std::nullopt;
            const Entity parent = e.GetParent();
            return EntityState{ SceneSerializer::EntityToJson(e, false), parent ? parent.GetGuid() : 0 };
        }

        // The authored document: workspace hooks collapse view-only state (exploded views) first.
        template<class F> void AsDocument(GameContext& ctx, F&& f) {
            Scene* scene = CurrentScene(ctx);
            if (!scene) return;
            WorkspaceExtension* ws = Internal::Workspace(ctx);
            const bool quad = IsQuadView(*scene);
            if (quad) SetQuadView(*scene, false);
            if (ws) ws->OnBeforeSave(ctx);
            f(*scene);
            if (ws) ws->OnAfterSave(ctx);
            if (quad) SetQuadView(*scene, true);
        }

        Baseline Capture(GameContext& ctx) {
            const auto t0 = std::chrono::steady_clock::now();
            Baseline b;
            AsDocument(ctx, [&](Scene& scene) {
                scene.Each<Transform>([&](Entity e, Transform&) {
                    if (scene.IsPendingDestroy(e)) return;
                    if (std::optional<EntityState> s = StateOf(e)) b.emplace(e.GetGuid(), std::move(*s));
                });
            });
            g.lastCaptureMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            return b;
        }

        void Rebase(GameContext& ctx) {
            g.baseline = Capture(ctx);
            g.baselineValid = true;
            g.committedSerial = g.editSerial;
        }

        size_t SizeOf(const Command& c) {
            size_t n = c.label.size() + sizeof(Command);
            for (const Change& ch : c.changes) n += sizeof(Change) + (ch.before ? ch.before->json.size() : 0) + (ch.after ? ch.after->json.size() : 0);
            return n;
        }

        std::string NameIn(const std::optional<EntityState>& s) {
            if (!s) return "entity";
            const size_t at = s->json.find("\"name\":\"");
            if (at == std::string::npos) return "entity";
            const size_t end = s->json.find('"', at + 8);
            return end == std::string::npos ? "entity" : s->json.substr(at + 8, end - at - 8);
        }

        // Top-level keys whose values differ between two entity JSON objects (cheap textual scan of
        // the keys SceneSerializer writes).
        std::vector<std::string> ChangedKeys(const std::string& a, const std::string& b) {
            std::vector<std::string> keys;
            std::vector<std::string> candidates = { "name", "transform", "materials" };
            for (const std::string& k : SceneSerializer::ComponentKeys()) candidates.push_back(k);
            auto value = [](const std::string& text, const std::string& key) -> std::string {
                const std::string tag = "\"" + key + "\":";
                const size_t at = text.find(tag);
                if (at == std::string::npos) return std::string(1, '\x01');   // absent
                // value ends at the next top-level key: good enough for a label (exact diff is the JSON compare)
                size_t depth = 0, i = at + tag.size();
                for (; i < text.size(); ++i) {
                    const char c = text[i];
                    if (c == '{' || c == '[') ++depth;
                    else if (c == '}' || c == ']') { if (depth == 0) break; --depth; }
                    else if (c == ',' && depth == 0) break;
                    else if (c == '"') { for (++i; i < text.size() && text[i] != '"'; ++i) if (text[i] == '\\') ++i; }
                }
                return text.substr(at, i - at);
            };
            for (const std::string& k : candidates) if (value(a, k) != value(b, k)) keys.push_back(k);
            return keys;
        }

        std::string LabelFor(const Command& c) {
            if (c.changes.empty()) return "Edit";
            if (c.changes.size() > 1) {
                size_t created = 0, deleted = 0;
                for (const Change& ch : c.changes) { created += !ch.before; deleted += !ch.after; }
                if (created == c.changes.size()) return "Create '" + NameIn(c.changes.front().after) + "'" + (created > 1 ? " (" + std::to_string(created) + " entities)" : "");
                if (deleted == c.changes.size()) return "Delete '" + NameIn(c.changes.front().before) + "'" + (deleted > 1 ? " (" + std::to_string(deleted) + " entities)" : "");
                return "Edit " + std::to_string(c.changes.size()) + " entities";
            }
            const Change& ch = c.changes.front();
            if (!ch.before) return "Create '" + NameIn(ch.after) + "'";
            if (!ch.after) return "Delete '" + NameIn(ch.before) + "'";
            const std::string name = NameIn(ch.after);
            if (ch.before->parent != ch.after->parent && ch.before->json == ch.after->json) return "Reparent '" + name + "'";
            const std::vector<std::string> keys = ChangedKeys(ch.before->json, ch.after->json);
            if (keys.size() == 1 && keys[0] == "name") return "Rename '" + NameIn(ch.before) + "' to '" + name + "'";
            if (keys.size() == 1 && keys[0] == "transform") {
                const int op = Internal::GizmoOperation();
                return std::string(op == ImGuizmo::ROTATE ? "Rotate" : op == ImGuizmo::SCALE ? "Scale" : "Move") + " '" + name + "'";
            }
            if (keys.size() == 1 && keys[0] == "materials") return "Edit material of '" + name + "'";
            if (keys.size() == 1) {
                const bool had = ch.before->json.find("\"" + keys[0] + "\":") != std::string::npos;
                const bool has = ch.after->json.find("\"" + keys[0] + "\":") != std::string::npos;
                if (!had && has) return "Add " + keys[0] + " to '" + name + "'";
                if (had && !has) return "Remove " + keys[0] + " from '" + name + "'";
                return "Edit " + keys[0] + " of '" + name + "'";
            }
            return "Edit '" + name + "'";
        }

        void Push(Command c) {
            c.bytes = SizeOf(c);
            g.bytes += c.bytes;
            g.undo.push_back(std::move(c));
            for (const Command& r : g.redo) g.bytes -= r.bytes;
            g.redo.clear();   // a new edit invalidates the redo branch
            while (!g.undo.empty() && (g.undo.size() > g.maxEntries || g.bytes > g.maxBytes)) {
                g.bytes -= g.undo.front().bytes;
                g.undo.pop_front();
            }
        }

        // Diffs the current document against the baseline; pushes a command if anything changed.
        bool Commit(GameContext& ctx, const std::string& label) {
            if (!g.baselineValid) { Rebase(ctx); return false; }
            Baseline now = Capture(ctx);
            Baseline before = g.baseline;
            if (g.selectedPre && g.selectedGuid) {   // the selected entity's state from before the interaction
                if (before.count(g.selectedGuid)) before[g.selectedGuid] = *g.selectedPre;
            }
            Command c;
            for (const auto& [guid, state] : now) {
                auto it = before.find(guid);
                if (it == before.end()) c.changes.push_back({ guid, std::nullopt, state });
                else if (!(it->second == state)) c.changes.push_back({ guid, it->second, state });
            }
            for (const auto& [guid, state] : before) if (!now.count(guid)) c.changes.push_back({ guid, state, std::nullopt });
            g.baseline = std::move(now);
            g.committedSerial = g.editSerial;
            if (c.changes.empty()) return false;
            c.label = label.empty() ? LabelFor(c) : label;
            Log::Info() << "Editor: " << c.label << " (" << c.changes.size() << " entit" << (c.changes.size() == 1 ? "y" : "ies") << ")";
            Push(std::move(c));
            return true;
        }

        // Brings the listed entities to their before (undo) or after (redo) state.
        bool Apply(GameContext& ctx, const Command& c, bool after) {
            Scene* scene = CurrentScene(ctx);
            if (!scene) return false;
            const bool quad = IsQuadView(*scene);
            if (quad) SetQuadView(*scene, false);
            bool ok = true;
            std::vector<std::pair<Entity, uint64_t>> parents;
            uint64_t select = 0;
            // 1. entities that must exist: create (with their guid) and apply their state
            for (const Change& ch : c.changes) {
                const std::optional<EntityState>& target = after ? ch.after : ch.before;
                if (!target) continue;
                Entity e = scene->FindByGuid(ch.guid);
                if (!e) { e = scene->CreateEntity(); scene->SetGuid(e, ch.guid); }
                std::string error;
                if (!SceneSerializer::ApplyEntityJson(e, target->json, &error)) { Log::Error() << "Editor: undo / redo could not restore an entity: " << error; ok = false; }
                parents.emplace_back(e, target->parent);
                if (!select) select = ch.guid;
            }
            // 2. hierarchy
            for (auto& [e, parentGuid] : parents) {
                const Entity parent = parentGuid ? scene->FindByGuid(parentGuid) : Entity();
                if (e.GetParent() != parent) e.SetParent(parent, false);
            }
            // 3. entities that must not exist (children that stay were re-parented above)
            for (const Change& ch : c.changes) {
                if ((after ? ch.after : ch.before)) continue;
                if (Entity e = scene->FindByGuid(ch.guid)) {
                    if (SelectedEntity() == e.Id() + 1) Select(0);
                    scene->DestroyImmediate(e);
                }
            }
            if (WorkspaceExtension* ws = Internal::Workspace(ctx)) ws->OnAfterLoad(ctx);   // document replaced: reset view-only state
            if (quad) SetQuadView(*scene, true);
            if (select) if (Entity e = scene->FindByGuid(select)) Select(e.Id() + 1);
            MarkEdited();
            Rebase(ctx);   // the applied state is the new baseline (not a new edit)
            return ok;
        }

        bool g_held = false;
        bool Interacting() {
            return g_held || ImGui::IsAnyItemActive() || ImGuizmo::IsUsing() || ImGui::GetDragDropPayload() != nullptr;
        }

        void InstallStateHook(GameContext& ctx) {
            // The engine's pause key (and the Simulation menu) change ctx.state; a Paused -> Running
            // change in edit mode first enters play mode, so the authored state is captured before
            // the first simulated step.
            ctx.beforeStateChange = [](GameContext& c, GameState from, GameState to) {
                if (g.mode == Mode::Edit && from == GameState::Paused && to == GameState::Running && UI::IsActive()) EnterPlay(c, nullptr, false);
            };
            g.hookInstalled = true;
        }
    }

    // ==================================================================== internal hooks
    namespace Internal {
        bool IsEditorEntity(const Entity& e) {
            const std::string& n = e.GetName();
            return n == "TopView" || n == "FrontView" || n == "SideView";
        }

        void HistoryNoteEdit() { ++g.editSerial; }

        void HistoryReset(GameContext& ctx) {
            Scene* scene = CurrentScene(ctx);
            g.undo.clear();
            g.redo.clear();
            g.bytes = 0;
            g.selectedPre.reset();
            g.selectedGuid = 0;
            g.sceneSerial = scene ? scene->Serial() : 0;
            g.mode = Mode::Edit;
            g.snapshot.clear();
            Rebase(ctx);
        }

        void HistoryBeginFrame(GameContext& ctx) {
            if (!g.hookInstalled) InstallStateHook(ctx);
            Scene* scene = CurrentScene(ctx);
            if (!scene) return;
            if (scene->Serial() != g.sceneSerial) HistoryReset(ctx);
            // Not during an interaction, and not when an edit made before the UI ran (code, automation)
            // is pending: the snapshot must be the state from before the edit.
            if (g.mode == Mode::Play || g.interacting || g.editSerial != g.committedSerial) return;
            const Entity sel = Internal::Selected(*scene);
            g.selectedGuid = sel ? sel.GetGuid() : 0;
            g.selectedPre.reset();
            if (sel) AsDocument(ctx, [&](Scene&) { g.selectedPre = StateOf(sel); });
        }

        void HistoryEndFrame(GameContext& ctx) {
            if (g.mode == Mode::Play && ImGui::GetCurrentContext()) {   // mode indication: amber frame + note on the viewport
                const glm::vec4 v = ViewportRect();
                const float s = GetLayout().scale;
                ImDrawList* dl = ImGui::GetForegroundDrawList();
                const ImU32 amber = ImGui::GetColorU32(Amber());
                dl->AddRect(ImVec2(v.x + 1.0f, v.y + 1.0f), ImVec2(v.x + v.z - 1.0f, v.y + v.w - 1.0f), amber, 0.0f, 0, 2.0f * s);
                dl->AddText(ImVec2(v.x + 10.0f * s, v.y + 8.0f * s), amber, ctx.state == GameState::Running ? "PLAY MODE  -  changes made now are discarded by Stop"
                                                                                                         : "PLAY MODE (PAUSED)  -  changes made now are discarded by Stop");
            }
            Scene* scene = CurrentScene(ctx);
            if (!scene || g.mode == Mode::Play) { g.interacting = false; return; }
            const bool interacting = Interacting();
            if (interacting && !g.interacting && g.editSerial == g.committedSerial) Rebase(ctx);   // an interaction begins: fresh baseline
            if (!interacting && !g.explicitOpen && g.editSerial != g.committedSerial) Commit(ctx, std::string());
            g.interacting = interacting;
        }

        double HistoryCaptureMs() { return g.lastCaptureMs; }
    }

    // ==================================================================== public API: history
    HistoryState History() {
        HistoryState h;
        h.canUndo = g.mode == Mode::Edit && !g.undo.empty();
        h.canRedo = g.mode == Mode::Edit && !g.redo.empty();
        if (!g.undo.empty()) h.undoLabel = g.undo.back().label;
        if (!g.redo.empty()) h.redoLabel = g.redo.back().label;
        h.undoCount = g.undo.size();
        h.redoCount = g.redo.size();
        h.bytes = g.bytes;
        h.lastCaptureMs = g.lastCaptureMs;
        return h;
    }

    bool Undo(GameContext& ctx) {
        if (g.mode != Mode::Edit || g.undo.empty() || !CurrentScene(ctx)) return false;
        if (g.editSerial != g.committedSerial) Commit(ctx, std::string());   // a pending edit is undone first
        if (g.undo.empty()) return false;
        Command c = std::move(g.undo.back());
        g.undo.pop_back();
        const bool ok = Apply(ctx, c, false);
        Log::Info() << "Editor: undo " << c.label;
        g.redo.push_back(std::move(c));
        return ok;
    }

    bool Redo(GameContext& ctx) {
        if (g.mode != Mode::Edit || g.redo.empty() || !CurrentScene(ctx)) return false;
        Command c = std::move(g.redo.back());
        g.redo.pop_back();
        const bool ok = Apply(ctx, c, true);
        Log::Info() << "Editor: redo " << c.label;
        g.undo.push_back(std::move(c));
        return ok;
    }

    void SetInteractionHeld(bool held) { g_held = held; }

    void ClearHistory() {
        g.undo.clear();
        g.redo.clear();
        g.bytes = 0;
    }

    void SetHistoryLimits(size_t maxEntries, size_t maxBytes) {
        g.maxEntries = std::max<size_t>(maxEntries, 1);
        g.maxBytes = std::max<size_t>(maxBytes, 1024);
        while (!g.undo.empty() && (g.undo.size() > g.maxEntries || g.bytes > g.maxBytes)) { g.bytes -= g.undo.front().bytes; g.undo.pop_front(); }
    }

    bool BeginEdit(GameContext& ctx, const std::string& label) {
        if (g.mode != Mode::Edit || !CurrentScene(ctx)) return false;
        if (g.editSerial != g.committedSerial) Commit(ctx, std::string());   // finish an edit in progress
        Rebase(ctx);
        g.explicitLabel = label;
        g.explicitOpen = true;
        g.selectedPre.reset();
        return true;
    }

    bool EndEdit(GameContext& ctx) {
        if (!g.explicitOpen) return false;
        g.explicitOpen = false;
        MarkEdited();
        const bool changed = Commit(ctx, g.explicitLabel);
        g.explicitLabel.clear();
        return changed;
    }

    // ==================================================================== public API: play mode
    Mode CurrentMode() { return g.mode; }

    bool EnterPlay(GameContext& ctx, std::string* status, bool setRunning) {
        Scene* scene = CurrentScene(ctx);
        if (!scene) { if (status) *status = "no scene"; return false; }
        if (g.mode == Mode::Play) { if (setRunning) ctx.state = GameState::Running; return true; }
        if (g.editSerial != g.committedSerial) Commit(ctx, std::string());
        AsDocument(ctx, [&](Scene& s) { g.snapshot = SceneSerializer::ToJson(s, false); });
        if (g.snapshot.empty()) { if (status) *status = "play: could not capture the scene"; return false; }
        const Entity sel = Internal::Selected(*scene);
        g.playSelection = sel ? sel.GetGuid() : 0;
        g.mode = Mode::Play;
        if (setRunning) ctx.state = GameState::Running;
        Log::Info() << "Editor: play (edit state captured, " << g.snapshot.size() / 1024 << " KiB)";
        if (status) *status = "playing: Stop returns to the edit state";
        return true;
    }

    bool StopPlay(GameContext& ctx, std::string* status) {
        Scene* scene = CurrentScene(ctx);
        if (g.mode != Mode::Play) { if (status) *status = "not playing"; return false; }
        ctx.state = GameState::Paused;
        g.mode = Mode::Edit;
        if (!scene) { g.snapshot.clear(); return false; }
        const bool quad = IsQuadView(*scene);
        if (quad) SetQuadView(*scene, false);
        std::string error;
        const bool ok = SceneSerializer::RestoreSnapshot(*scene, g.snapshot, &error);
        Physics::Advanced::ResetSimulations();   // particles / soft bodies restart from the restored components
        ctx.replay.Clear();
        if (WorkspaceExtension* ws = Internal::Workspace(ctx)) ws->OnAfterLoad(ctx);
        if (quad) SetQuadView(*scene, true);
        Select(0);
        if (g.playSelection) if (Entity e = scene->FindByGuid(g.playSelection)) Select(e.Id() + 1);
        g.snapshot.clear();
        Rebase(ctx);
        if (ok) Log::Info() << "Editor: stop (edit state restored)";
        else Log::Error() << "Editor: stop could not restore the edit state: " << error;
        if (status) *status = ok ? "stopped: edit state restored" : "stop failed: " + error;
        return ok;
    }
}
