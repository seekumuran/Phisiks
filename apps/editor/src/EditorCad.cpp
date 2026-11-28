// CAD parts in the editor: regeneration (CadSync), the CAD PART inspector section (parameters,
// feature history, feature properties, inspection, export) and the viewport tools (sketch editor on
// the sketch plane, face / edge picking for fillets, chamfers and measurement).
//
// A part is an entity with a cadModel component; its document (Phisiks/Cad/Document.h) is the
// authored data. Every edit made here rewrites cadModel.document and notes an edit, so the editor's
// undo / redo, saving, play-mode snapshots and the agent interface handle CAD like any other
// component. CadSync regenerates a part when its document revision changes and replaces the mesh
// CadModel::MeshName(guid), which the renderer draws for the entity.
#include "EditorInternal.h"

#include "Phisiks/Assets/AssetDatabase.h"
#include "Phisiks/Cad/Document.h"
#include "Phisiks/Cad/Expression.h"
#include "Phisiks/Cad/Kernel.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/SceneSerializer.h"
#include "Phisiks/UI/ImGuiLayer.h"
#include "Phisiks/UI/RetroDeck.h"

#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <unordered_map>

using namespace Phisiks;
namespace fs = std::filesystem;
namespace Cad = Phisiks::Cad;
namespace Retro = Phisiks::UI::Retro;

namespace Editor {
    namespace {
        constexpr double PI = 3.14159265358979323846;

        struct Part {
            uint64_t revision = 0;
            Cad::Document doc;
            std::string loadError;              // the document JSON did not parse
            Cad::RegenerateResult result;
            Cad::Tessellation mesh;             // document units
            Cad::Properties properties;
            double buildMs = 0.0;
            bool hasMesh = false;
        };
        std::unordered_map<uint64_t, Part> g_parts;      // by entity guid

        // ---- UI state
        uint64_t g_partGuid = 0;            // the part the UI state belongs to
        Cad::Id g_feature = 0;              // selected feature
        bool g_sketching = false;           // editing g_feature (a sketch) in the viewport
        enum class Tool { Select, Point, Line, Rectangle, Circle, Arc };
        Tool g_tool = Tool::Select;
        std::vector<Cad::Id> g_selection;   // sketch geometry
        std::vector<glm::dvec2> g_clicks;   // pending clicks of the current tool
        Cad::Id g_chain = 0;                // line tool: last point
        Cad::Id g_dragPoint = 0;
        bool g_construction = false;
        bool g_gridSnap = true;
        double g_grid = 1.0;                // document units
        enum class Pick { None, Edges, Measure };
        Pick g_pick = Pick::None;
        struct PickHit { int face = 0, edge = 0; glm::dvec3 point{ 0.0 }; };
        PickHit g_measure[2];
        int g_measureCount = 0;
        Cad::Tessellation g_pickMesh;       // shape the picks refer to (the fillet's input body, or the result)
        Cad::Shape g_pickShape;
        Cad::Id g_pickFeature = 0;
        std::string g_status;
        std::string g_newParam = "", g_newParamExpr = "10";

        // Automation: screen rects of the CAD controls drawn this frame (Editor::UiItemRect).
        std::map<std::string, glm::vec4> g_items, g_itemsDrawn;
        void Track(const std::string& id) {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            if (ImGui::IsItemVisible()) g_itemsDrawn[id] = glm::vec4(a.x, a.y, b.x - a.x, b.y - a.y);
        }

        std::string Hex(uint64_t v) { char t[20]; std::snprintf(t, sizeof(t), "%016llx", static_cast<unsigned long long>(v)); return t; }

        bool LoadDocument(const CadModel& cad, Cad::Document& doc, std::string* error) {
            if (cad.document.empty()) { doc = Cad::Document(); return true; }
            return Cad::Document::FromJson(cad.document, doc, error);
        }
        std::string ResolveAsset(const std::string& reference) {
            if (ProjectDirectory().empty()) return {};
            return ProjectAssets().Resolve(reference);
        }

        void Build(Entity e, CadModel& cad, Part& part) {
            const auto t0 = std::chrono::steady_clock::now();
            part.revision = cad.revision;
            part.loadError.clear();
            part.result = {};
            part.mesh = {};
            part.properties = {};
            std::string error;
            if (!LoadDocument(cad, part.doc, &error)) { part.loadError = error; part.doc = Cad::Document(); }
            else {
                part.result = part.doc.Regenerate(ResolveAsset);
                const Cad::Shape shape = part.doc.Result();
                if (shape) {
                    part.properties = Cad::Kernel::Measure(shape);
                    const double diag = glm::length(part.properties.boundsMax - part.properties.boundsMin);
                    part.mesh = Cad::Kernel::Tessellate(shape, std::max(diag * 0.0015, 1e-4), 0.3);
                }
            }
            const std::string name = CadModel::MeshName(e.GetGuid());
            if (!part.mesh.indices.empty()) {
                const float scale = float(Cad::MetresPerUnit(part.doc.unit));
                Render::MeshData data;
                data.vertices.resize(part.mesh.positions.size());
                for (size_t i = 0; i < data.vertices.size(); ++i) {
                    data.vertices[i].position = part.mesh.positions[i] * scale;
                    data.vertices[i].normal = part.mesh.normals[i];
                    data.vertices[i].uv = glm::vec2(part.mesh.positions[i].x, part.mesh.positions[i].y) * scale;
                }
                data.indices = part.mesh.indices;
                part.hasMesh = Render::ReplaceMesh(name, data);
            }
            else {
                Render::ReleaseMesh(name);
                part.hasMesh = false;
            }
            cad.meshReady = part.hasMesh;
            cad.builtRevision = cad.revision;
            part.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (!part.loadError.empty()) Log::Warning() << "CAD: '" << e.GetName() << "': " << part.loadError;
            else if (!part.result.ok && !part.result.firstError.empty()) Log::Info() << "CAD: '" << e.GetName() << "': " << part.result.firstError;
        }

        Part* PartOf(Entity e) {
            if (!e || !e.Has<CadModel>()) return nullptr;
            auto it = g_parts.find(e.GetGuid());
            return it == g_parts.end() ? nullptr : &it->second;
        }

        // An edit of the part: the new document becomes the component (one undo step per interaction).
        void Commit(Entity e, const Cad::Document& doc) {
            CadModel& cad = e.Get<CadModel>();
            const std::string text = doc.ToJson();
            if (text == cad.document) return;
            cad.document = text;
            ++cad.revision;
            Internal::NoteEdit();
        }

        double Value(const Cad::Document& doc, const std::string& expr, double fallback = 0.0) {
            double v = fallback;
            if (!doc.Eval(expr, v)) return fallback;
            return v;
        }

        // the sketch with its dimension expressions applied and solved (what the build uses)
        void PrepareSketch(const Cad::Document& doc, Cad::Sketch& s) {
            for (const Cad::Constraint& c : s.Constraints()) {
                if (c.expression.empty()) continue;
                double v;
                if (doc.Eval(c.expression, v)) s.SetConstraintValue(c.id, v);
            }
            s.Solve();
        }

        Cad::Plane SketchPlane(const Cad::Document& doc, const Cad::Feature& f) {
            Cad::Plane p = f.plane == "XZ" ? Cad::Plane::XZ() : f.plane == "YZ" ? Cad::Plane::YZ() : f.plane == "custom" ? f.frame : Cad::Plane::XY();
            p.origin += p.Normal() * Value(doc, f.values.count("offset") ? f.values.at("offset") : "0");
            return p;
        }

        // document units -> world
        glm::mat4 PartMatrix(Entity e, const Cad::Document& doc) {
            return e.WorldMatrix() * glm::scale(glm::mat4(1.0f), glm::vec3(float(Cad::MetresPerUnit(doc.unit))));
        }

        // ---- screen projection helpers
        struct ViewInfo {
            glm::mat4 viewProj{ 1.0f }, inverse{ 1.0f };
            glm::vec4 rect{ 0.0f };
            bool valid = false;
        };
        ViewInfo View(Scene& scene) {
            ViewInfo v;
            glm::mat4 view, proj;
            if (!Internal::CameraMatrices(scene, view, proj, v.rect)) return v;
            v.viewProj = proj * view;
            v.inverse = glm::inverse(v.viewProj);
            v.valid = true;
            return v;
        }
        bool ToScreen(const ViewInfo& v, const glm::vec3& world, ImVec2& out) {
            const glm::vec4 c = v.viewProj * glm::vec4(world, 1.0f);
            if (c.w <= 1e-6f) return false;
            const glm::vec3 n = glm::vec3(c) / c.w;
            out = ImVec2(v.rect.x + (n.x * 0.5f + 0.5f) * v.rect.z, v.rect.y + (0.5f - n.y * 0.5f) * v.rect.w);
            return true;
        }
        void MouseRay(const ViewInfo& v, glm::vec3& origin, glm::vec3& dir) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            const float nx = (m.x - v.rect.x) / v.rect.z * 2.0f - 1.0f, ny = 1.0f - (m.y - v.rect.y) / v.rect.w * 2.0f;
            glm::vec4 a = v.inverse * glm::vec4(nx, ny, -1.0f, 1.0f), b = v.inverse * glm::vec4(nx, ny, 1.0f, 1.0f);
            a /= a.w; b /= b.w;
            origin = glm::vec3(a);
            dir = glm::normalize(glm::vec3(b - a));
        }
        float SegmentDistance(ImVec2 p, ImVec2 a, ImVec2 b) {
            const float dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
            float t = len2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            const float x = a.x + t * dx - p.x, y = a.y + t * dy - p.y;
            return std::sqrt(x * x + y * y);
        }

        const char* FeatureIcon(Cad::FeatureType t) {
            switch (t) {
                case Cad::FeatureType::Sketch: return "[S]";
                case Cad::FeatureType::Extrude: return "[E]";
                case Cad::FeatureType::Revolve: return "[R]";
                case Cad::FeatureType::Box: case Cad::FeatureType::Cylinder: case Cad::FeatureType::Sphere: return "[P]";
                case Cad::FeatureType::Fillet: return "[F]";
                case Cad::FeatureType::Chamfer: return "[C]";
                case Cad::FeatureType::Boolean: return "[B]";
                case Cad::FeatureType::LinearPattern: case Cad::FeatureType::CircularPattern: return "[#]";
                case Cad::FeatureType::Mirror: return "[M]";
                case Cad::FeatureType::Import: return "[I]";
            }
            return "[?]";
        }

        void ResetTools() {
            g_sketching = false;
            g_tool = Tool::Select;
            g_selection.clear();
            g_clicks.clear();
            g_chain = 0;
            g_dragPoint = 0;
            g_pick = Pick::None;
            g_measureCount = 0;
            g_pickFeature = 0;
            g_pickShape = {};
            g_pickMesh = {};
            SetInteractionHeld(false);
        }

        // Expression input: commits on Enter or when focus leaves after an edit.
        bool ExprInput(const char* label, std::string& value, float width = -1.0f) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", value.c_str());
            ImGui::SetNextItemWidth(width);
            const bool enter = ImGui::InputText(label, buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue);
            Track(std::string("input:") + label);
            if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
                if (value != buf) { value = buf; return true; }
            }
            return false;
        }
        bool Vec3Input(const char* label, glm::dvec3& v) {
            glm::vec3 f(v);
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputFloat3(label, &f.x, "%.3f");
            if (ImGui::IsItemDeactivatedAfterEdit()) { v = glm::dvec3(f); return true; }
            return false;
        }
        bool BodyCombo(const char* label, const Part& part, Cad::Id& body, Cad::Id exclude = 0) {
            std::string current = "(latest body)";
            for (const Cad::Feature& f : part.doc.Features()) if (f.id == body) current = f.name;
            bool changed = false;
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo(label, current.c_str())) {
                if (ImGui::Selectable("(latest body)", body == 0)) { body = 0; changed = true; }
                for (const Cad::Feature& f : part.doc.Features()) {
                    const bool makes = f.type == Cad::FeatureType::Extrude || f.type == Cad::FeatureType::Revolve || f.type == Cad::FeatureType::Box
                        || f.type == Cad::FeatureType::Cylinder || f.type == Cad::FeatureType::Sphere || f.type == Cad::FeatureType::Import;
                    if (!makes || f.op != Cad::FeatureOp::NewBody || f.id == exclude) continue;
                    if (ImGui::Selectable(f.name.c_str(), body == f.id)) { body = f.id; changed = true; }
                }
                ImGui::EndCombo();
            }
            return changed;
        }

        std::string Fmt(double v, const Cad::Document& doc, int power = 1) {
            char t[64];
            const char* unit = Cad::UnitSymbol(doc.unit);
            if (power == 1) std::snprintf(t, sizeof(t), "%.4g %s", v, unit);
            else std::snprintf(t, sizeof(t), "%.6g %s%s", v, unit, power == 2 ? "^2" : "^3");
            return t;
        }

        // ================================================================ feature editors
        void SketchEditor(Entity e, Part& part, Cad::Feature& f, bool& changed) {
            const char* planes[] = { "XY", "XZ", "YZ" };
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("plane", f.plane.c_str())) {
                for (const char* p : planes) if (ImGui::Selectable(p, f.plane == p)) { f.plane = p; changed = true; }
                ImGui::EndCombo();
            }
            changed |= ExprInput("offset", f.values["offset"]);
            Cad::Sketch preview = f.sketch;
            PrepareSketch(part.doc, preview);
            const Cad::SolveResult& sr = preview.LastSolve();
            const ImVec4 colour = sr.status != Cad::SolveStatus::Solved ? ImVec4(1.0f, 0.35f, 0.3f, 1.0f) : sr.degreesOfFreedom == 0 ? Internal::Phosphor() : Internal::Amber();
            ImGui::TextColored(colour, "%s", sr.message.c_str());
            ImGui::TextDisabled("%zu geometry, %zu constraints", f.sketch.Geometries().size(), f.sketch.Constraints().size());
            if (!g_sketching) { const bool edit = Retro::Button("EDIT SKETCH", -1.0f, 1); Track("EDIT SKETCH"); if (edit) { ResetTools(); g_sketching = true; g_feature = f.id; } }
            else { const bool finish = Retro::Button("FINISH SKETCH", -1.0f, 1); Track("FINISH SKETCH"); if (finish) ResetTools(); }
            // dimensions: value or expression
            if (ImGui::TreeNodeEx("Dimensions and constraints", ImGuiTreeNodeFlags_DefaultOpen)) {
                Cad::Id remove = 0;
                for (const Cad::Constraint& c : f.sketch.Constraints()) {
                    ImGui::PushID(int(c.id));
                    if (ImGui::SmallButton("x")) remove = c.id;
                    ImGui::SameLine();
                    if (Cad::IsDimension(c.type)) {
                        std::string text = c.expression.empty() ? [&] { char t[32]; std::snprintf(t, sizeof(t), "%.6g", c.value); return std::string(t); }() : c.expression;
                        ImGui::TextUnformatted(Cad::ToString(c.type));
                        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
                        const bool dimEdited = ExprInput("##dim", text);
                        Track("dim:" + std::to_string(c.id));
                        if (dimEdited) {
                            char* end = nullptr;
                            const double v = std::strtod(text.c_str(), &end);
                            Cad::Sketch trial = f.sketch;
                            bool ok;
                            if (end && *end == 0 && !text.empty()) { ok = trial.SetConstraintValue(c.id, v); trial.SetConstraintExpression(c.id, ""); }
                            else {
                                double ev;
                                std::string err;
                                ok = part.doc.Eval(text, ev, &err) && trial.SetConstraintValue(c.id, ev);
                                if (ok) trial.SetConstraintExpression(c.id, text);
                                else g_status = err.empty() ? "invalid value" : err;
                            }
                            if (ok) {
                                PrepareSketch(part.doc, trial);
                                if (trial.LastSolve().status == Cad::SolveStatus::Solved) { f.sketch = trial; changed = true; g_status.clear(); }
                                else g_status = "not applied: " + trial.LastSolve().message;
                            }
                        }
                    }
                    else ImGui::TextUnformatted(Cad::ToString(c.type));
                    ImGui::PopID();
                }
                if (remove) { f.sketch.RemoveConstraint(remove); changed = true; }
                ImGui::TreePop();
            }
            (void)e;
        }

        void ProfileFeatureEditor(Part& part, Cad::Feature& f, bool& changed) {
            std::string current = "(choose a sketch)";
            for (const Cad::Feature& s : part.doc.Features()) if (s.id == f.profileSketch) current = s.name;
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("sketch", current.c_str())) {
                for (const Cad::Feature& s : part.doc.Features()) {
                    if (s.type != Cad::FeatureType::Sketch) continue;
                    if (part.doc.IndexOf(s.id) >= part.doc.IndexOf(f.id)) break;
                    if (ImGui::Selectable(s.name.c_str(), s.id == f.profileSketch)) { f.profileSketch = s.id; changed = true; }
                }
                ImGui::EndCombo();
            }
            if (f.type == Cad::FeatureType::Extrude) {
                changed |= ExprInput("distance", f.values["distance"]);
                changed |= ImGui::Checkbox("symmetric", &f.symmetric); ImGui::SameLine();
                changed |= ImGui::Checkbox("reverse", &f.reverse);
            }
            else {
                changed |= ExprInput("angle (deg)", f.values["angle"]);
                const char* axes[] = { "x", "y", "line" };
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("axis", f.axis == "line" ? "sketch line" : (std::string("sketch ") + f.axis).c_str())) {
                    for (const char* a : axes) if (ImGui::Selectable(std::string(a) == "line" ? "sketch line (selected in the sketch)" : (std::string("sketch ") + a).c_str(), f.axis == a)) { f.axis = a; changed = true; }
                    ImGui::EndCombo();
                }
                if (f.axis == "line") {
                    ImGui::TextDisabled("axis line id %u", f.axisLine);
                    if (ImGui::SmallButton("use the selected sketch line")) {
                        for (Cad::Id id : g_selection) {
                            const Cad::Feature* s = part.doc.FindFeature(f.profileSketch);
                            const Cad::Geometry* g = s ? s->sketch.Find(id) : nullptr;
                            if (g && g->type == Cad::GeometryType::Line) { f.axisLine = id; changed = true; break; }
                        }
                    }
                }
                changed |= ImGui::Checkbox("reverse", &f.reverse);
            }
        }

        bool OpCombo(Cad::Feature& f) {
            const Cad::FeatureOp ops[] = { Cad::FeatureOp::NewBody, Cad::FeatureOp::Join, Cad::FeatureOp::Cut, Cad::FeatureOp::Intersect };
            const char* names[] = { "new body", "join", "cut", "intersect" };
            bool changed = false;
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("operation", names[int(f.op)])) {
                for (int i = 0; i < 4; ++i) if (ImGui::Selectable(names[i], f.op == ops[i])) { f.op = ops[i]; changed = true; }
                ImGui::EndCombo();
            }
            return changed;
        }

        void BeginEdgePick(Part& part, const Cad::Feature& f) {
            g_pick = Pick::Edges;
            g_pickFeature = f.id;
            g_pickShape = part.doc.ShapeBefore(f.id);
            if (!g_pickShape) {   // not built yet: the body it would start from
                const std::vector<Cad::Id> deps = part.doc.Dependencies(f);
                for (Cad::Id d : deps) if (const Cad::Shape s = part.doc.ShapeAfter(d)) g_pickShape = s;
            }
            const Cad::Properties p = Cad::Kernel::Measure(g_pickShape);
            g_pickMesh = Cad::Kernel::Tessellate(g_pickShape, std::max(glm::length(p.boundsMax - p.boundsMin) * 0.0015, 1e-4), 0.3);
            g_status = g_pickShape ? "click edges in the viewport to add / remove them" : "the body is not built: fix the earlier features first";
        }

        void FeatureEditor(Entity e, Part& part) {
            Cad::Feature* live = part.doc.FindFeature(g_feature);
            if (!live) return;
            Cad::Feature f = *live;
            bool changed = false;
            ImGui::PushID(int(f.id));
            char name[96];
            std::snprintf(name, sizeof(name), "%s", f.name.c_str());
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##fname", name, sizeof(name));
            if (ImGui::IsItemDeactivatedAfterEdit() && f.name != name && name[0]) { f.name = name; changed = true; }
            const Cad::FeatureState* st = part.doc.State(f.id);
            if (st && !st->error.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.35f, 1.0f)); ImGui::TextWrapped("%s", st->error.c_str()); ImGui::PopStyleColor(); }
            else if (st && st->skipped) ImGui::TextColored(Internal::Amber(), "skipped: depends on a feature that failed");
            switch (f.type) {
                case Cad::FeatureType::Sketch: SketchEditor(e, part, f, changed); break;
                case Cad::FeatureType::Extrude: case Cad::FeatureType::Revolve:
                    ProfileFeatureEditor(part, f, changed);
                    changed |= OpCombo(f);
                    if (f.op != Cad::FeatureOp::NewBody) changed |= BodyCombo("target body", part, f.body);
                    break;
                case Cad::FeatureType::Box:
                    changed |= ExprInput("x", f.values["x"]); changed |= ExprInput("y", f.values["y"]); changed |= ExprInput("z", f.values["z"]);
                    ImGui::TextDisabled("corner"); changed |= Vec3Input("##pos", f.position);
                    changed |= OpCombo(f);
                    if (f.op != Cad::FeatureOp::NewBody) changed |= BodyCombo("target body", part, f.body);
                    break;
                case Cad::FeatureType::Cylinder:
                    changed |= ExprInput("radius", f.values["radius"]); changed |= ExprInput("height", f.values["height"]);
                    ImGui::TextDisabled("base centre"); changed |= Vec3Input("##pos", f.position);
                    ImGui::TextDisabled("axis"); changed |= Vec3Input("##dir", f.direction);
                    changed |= OpCombo(f);
                    if (f.op != Cad::FeatureOp::NewBody) changed |= BodyCombo("target body", part, f.body);
                    break;
                case Cad::FeatureType::Sphere:
                    changed |= ExprInput("radius", f.values["radius"]);
                    ImGui::TextDisabled("centre"); changed |= Vec3Input("##pos", f.position);
                    changed |= OpCombo(f);
                    if (f.op != Cad::FeatureOp::NewBody) changed |= BodyCombo("target body", part, f.body);
                    break;
                case Cad::FeatureType::Fillet: case Cad::FeatureType::Chamfer:
                    changed |= ExprInput(f.type == Cad::FeatureType::Fillet ? "radius" : "distance", f.values[f.type == Cad::FeatureType::Fillet ? "radius" : "distance"]);
                    changed |= BodyCombo("body", part, f.body);
                    ImGui::Text("%s", f.edges.empty() ? "every edge" : (std::to_string(f.edges.size()) + " edge(s)").c_str());
                    if (g_pick == Pick::Edges && g_pickFeature == f.id) { if (Retro::Button("DONE PICKING", -1.0f, 1)) { g_pick = Pick::None; g_status.clear(); } }
                    else if (Retro::Button("PICK EDGES", -1.0f)) BeginEdgePick(part, f);
                    if (!f.edges.empty() && ImGui::SmallButton("use every edge")) { f.edges.clear(); changed = true; }
                    break;
                case Cad::FeatureType::Boolean: {
                    const char* names[] = { "union", "subtract", "intersect" };
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::BeginCombo("boolean", names[int(f.boolean)])) {
                        for (int i = 0; i < 3; ++i) if (ImGui::Selectable(names[i], int(f.boolean) == i)) { f.boolean = Cad::BooleanOp(i); changed = true; }
                        ImGui::EndCombo();
                    }
                    changed |= BodyCombo("target body", part, f.body, f.tool);
                    Cad::Id tool = f.tool;
                    if (BodyCombo("tool body", part, tool, f.body) && tool) { f.tool = tool; changed = true; }
                    break;
                }
                case Cad::FeatureType::LinearPattern: case Cad::FeatureType::CircularPattern: case Cad::FeatureType::Mirror: {
                    if (f.type != Cad::FeatureType::Mirror) changed |= ExprInput("count", f.values["count"]);
                    if (f.type == Cad::FeatureType::LinearPattern) { changed |= ExprInput("spacing", f.values["spacing"]); ImGui::TextDisabled("direction"); changed |= Vec3Input("##dir", f.direction); }
                    if (f.type == Cad::FeatureType::CircularPattern) {
                        changed |= ExprInput("angle (deg)", f.values["angle"]);
                        ImGui::TextDisabled("axis origin"); changed |= Vec3Input("##org", f.origin);
                        ImGui::TextDisabled("axis direction"); changed |= Vec3Input("##dir", f.direction);
                    }
                    if (f.type == Cad::FeatureType::Mirror) {
                        ImGui::TextDisabled("plane origin"); changed |= Vec3Input("##org", f.origin);
                        ImGui::TextDisabled("plane normal"); changed |= Vec3Input("##dir", f.direction);
                        changed |= ImGui::Checkbox("keep the original", &f.keepOriginal);
                    }
                    if (f.type != Cad::FeatureType::Mirror) {
                        std::string current = "(the whole body)";
                        for (const Cad::Feature& s : part.doc.Features()) if (s.id == f.source) current = "repeat " + s.name;
                        ImGui::SetNextItemWidth(-1.0f);
                        if (ImGui::BeginCombo("pattern", current.c_str())) {
                            if (ImGui::Selectable("(the whole body)", f.source == 0)) { f.source = 0; changed = true; }
                            for (const Cad::Feature& s : part.doc.Features()) {
                                if (part.doc.IndexOf(s.id) >= part.doc.IndexOf(f.id)) break;
                                const bool tool = s.type == Cad::FeatureType::Extrude || s.type == Cad::FeatureType::Revolve || s.type == Cad::FeatureType::Box || s.type == Cad::FeatureType::Cylinder || s.type == Cad::FeatureType::Sphere;
                                if (tool && ImGui::Selectable(("repeat " + s.name).c_str(), f.source == s.id)) { f.source = s.id; changed = true; }
                            }
                            ImGui::EndCombo();
                        }
                    }
                    if (!f.source) changed |= BodyCombo("body", part, f.body);
                    break;
                }
                case Cad::FeatureType::Import:
                    ImGui::TextWrapped("from %s", f.asset.c_str());
                    ImGui::TextDisabled(f.brep.empty() ? "geometry read from the asset" : "exact geometry stored in the part (%zu KB)", f.brep.size() / 1024);
                    if (!f.asset.empty() && Retro::Button("RELOAD FROM ASSET", -1.0f)) {
                        const std::string path = ResolveAsset(f.asset);
                        std::string err;
                        const Cad::Shape s = path.empty() ? Cad::Shape() : Cad::Document::ImportFile(path, part.doc.unit, &err);
                        if (s) { f.brep = Cad::Kernel::ToBrep(s); changed = true; g_status = "reloaded " + f.asset; }
                        else g_status = path.empty() ? "the asset is missing" : err;
                    }
                    changed |= OpCombo(f);
                    if (f.op != Cad::FeatureOp::NewBody) changed |= BodyCombo("target body", part, f.body);
                    break;
            }
            if (st && st->built && st->properties.valid) ImGui::TextDisabled("%.1f ms, %d faces", st->milliseconds, st->properties.faces);
            ImGui::PopID();
            if (changed) {
                std::string error;
                if (part.doc.UpdateFeature(f, &error)) Commit(e, part.doc);
                else g_status = error;
            }
        }

        void AddFeatureMenu(Entity e, Part& part) {
            const bool open = ImGui::BeginCombo("##addfeature", "add feature...");
            Track("add feature");
            if (!open) return;
            Cad::Feature f;
            bool add = false;
            auto item = [&](const char* label, Cad::FeatureType t) { const bool hit = ImGui::Selectable(label); Track(std::string("add:") + label); if (hit) { f.type = t; add = true; } };
            const char* planes[] = { "XY", "XZ", "YZ" };
            for (const char* p : planes) {
                const std::string label = std::string("Sketch on ") + p;
                const bool hit = ImGui::Selectable(label.c_str());
                Track("add:" + label);
                if (hit) { f.type = Cad::FeatureType::Sketch; f.plane = p; add = true; }
            }
            ImGui::Separator();
            item("Extrude", Cad::FeatureType::Extrude);
            item("Revolve", Cad::FeatureType::Revolve);
            item("Box", Cad::FeatureType::Box);
            item("Cylinder", Cad::FeatureType::Cylinder);
            item("Sphere", Cad::FeatureType::Sphere);
            ImGui::Separator();
            item("Fillet", Cad::FeatureType::Fillet);
            item("Chamfer", Cad::FeatureType::Chamfer);
            item("Boolean", Cad::FeatureType::Boolean);
            item("Linear pattern", Cad::FeatureType::LinearPattern);
            item("Circular pattern", Cad::FeatureType::CircularPattern);
            item("Mirror", Cad::FeatureType::Mirror);
            ImGui::EndCombo();
            if (!add) return;
            if (f.type == Cad::FeatureType::Extrude || f.type == Cad::FeatureType::Revolve) {   // the latest sketch
                for (const Cad::Feature& s : part.doc.Features()) if (s.type == Cad::FeatureType::Sketch) f.profileSketch = s.id;
                if (!f.profileSketch) { g_status = "add a sketch with a closed profile first"; return; }
                bool hasBody = false;
                for (const Cad::Body& b : part.doc.Bodies()) { (void)b; hasBody = true; }
                if (hasBody && f.type == Cad::FeatureType::Extrude) f.op = Cad::FeatureOp::Join;
            }
            if (f.type == Cad::FeatureType::Boolean) {
                const auto& bodies = part.doc.Bodies();
                if (bodies.size() < 2) { g_status = "a boolean needs two bodies"; return; }
                f.body = bodies[bodies.size() - 2].id;
                f.tool = bodies.back().id;
            }
            if (f.type == Cad::FeatureType::Mirror) { f.direction = { 1, 0, 0 }; }
            if (f.type == Cad::FeatureType::LinearPattern) f.direction = { 1, 0, 0 };
            if (f.type == Cad::FeatureType::CircularPattern) f.direction = { 0, 0, 1 };
            const size_t at = std::min(part.doc.rollback, part.doc.Features().size());
            const Cad::Id id = part.doc.InsertFeature(f, at);
            if (!id) { g_status = "that feature needs something to work on (a sketch or a body) before it"; return; }
            if (part.doc.rollback != size_t(-1)) part.doc.rollback = at + 1;
            ResetTools();
            g_feature = id;
            if (f.type == Cad::FeatureType::Sketch) g_sketching = true;
            Commit(e, part.doc);
        }

        void Inspection(Part& part) {
            const Cad::Properties& p = part.properties;
            if (!part.doc.Result()) { ImGui::TextDisabled("no solid yet"); return; }
            ImGui::Text("volume   %s", Fmt(p.volume, part.doc, 3).c_str());
            ImGui::Text("area     %s", Fmt(p.area, part.doc, 2).c_str());
            const glm::dvec3 size = p.boundsMax - p.boundsMin;
            ImGui::Text("size     %.4g x %.4g x %.4g %s", size.x, size.y, size.z, Cad::UnitSymbol(part.doc.unit));
            ImGui::Text("centroid %.4g, %.4g, %.4g", p.centroid.x, p.centroid.y, p.centroid.z);
            ImGui::TextDisabled("%d solid(s), %d faces, %d edges, %d vertices, %s", p.solids, p.faces, p.edges, p.vertices, p.valid ? "valid" : "INVALID");
            if (g_pick == Pick::Measure) {
                if (Retro::Button("STOP MEASURING", -1.0f, 1)) { g_pick = Pick::None; g_measureCount = 0; }
            }
            else if (Retro::Button("MEASURE", -1.0f)) {
                ResetTools();
                g_pick = Pick::Measure;
                g_pickShape = part.doc.Result();
                g_pickMesh = part.mesh;
                g_status = "click faces or edges; two picks show the distance";
            }
            if (g_pick != Pick::Measure) return;
            const std::vector<Cad::FaceInfo> faces = Cad::Kernel::FacesOf(g_pickShape);
            const std::vector<Cad::EdgeInfo> edges = Cad::Kernel::EdgesOf(g_pickShape);
            for (int i = 0; i < g_measureCount; ++i) {
                const PickHit& h = g_measure[i];
                if (h.edge && h.edge <= int(edges.size())) {
                    const Cad::EdgeInfo& ed = edges[size_t(h.edge - 1)];
                    ImGui::Text("%c: edge %d, %s, length %s%s", 'A' + i, h.edge, Cad::ToString(ed.curve), Fmt(ed.length, part.doc).c_str(),
                                ed.curve == Cad::CurveType::Circle ? (", radius " + Fmt(ed.radius, part.doc)).c_str() : "");
                }
                else if (h.face && h.face <= int(faces.size())) {
                    const Cad::FaceInfo& fa = faces[size_t(h.face - 1)];
                    ImGui::Text("%c: face %d, %s, area %s", 'A' + i, h.face, Cad::ToString(fa.surface), Fmt(fa.area, part.doc, 2).c_str());
                    if (fa.radius > 0) ImGui::Text("   radius %s", Fmt(fa.radius, part.doc).c_str());
                    if (fa.surface == Cad::SurfaceType::Plane) ImGui::Text("   normal %.3f, %.3f, %.3f", fa.normal.x, fa.normal.y, fa.normal.z);
                }
            }
            if (g_measureCount == 2) {
                auto sub = [&](const PickHit& h) { return h.edge ? Cad::Kernel::EdgeShape(g_pickShape, h.edge) : Cad::Kernel::FaceShape(g_pickShape, h.face); };
                double d = 0;
                if (Cad::Kernel::Distance(sub(g_measure[0]), sub(g_measure[1]), d)) ImGui::TextColored(Internal::Phosphor(), "distance A-B  %s", Fmt(d, part.doc).c_str());
                if (g_measure[0].face && g_measure[1].face && !g_measure[0].edge && !g_measure[1].edge) {
                    const glm::dvec3 n0 = faces[size_t(g_measure[0].face - 1)].normal, n1 = faces[size_t(g_measure[1].face - 1)].normal;
                    if (glm::length(n0) > 0 && glm::length(n1) > 0)
                        ImGui::Text("angle between normals %.3f deg", std::acos(std::clamp(glm::dot(n0, n1), -1.0, 1.0)) * 180.0 / PI);
                }
            }
        }

        bool ExportPart(Entity e, Part& part, const std::string& format, std::string* path, std::string* status) {
            if (ProjectDirectory().empty()) ProjectAssets();   // opens the default project
            if (ProjectDirectory().empty()) { if (status) *status = "no project folder"; return false; }
            std::string stem;
            for (char c : e.GetName()) stem += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
            if (stem.empty()) stem = "part";
            const fs::path dir = fs::path(ProjectDirectory()) / "assets" / "exports";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string ext = format == "iges" ? "igs" : format == "phcad" ? "phcad" : format;
            const fs::path file = dir / (stem + "." + ext);
            std::string error;
            bool ok;
            if (format == "phcad") {
                std::ofstream out(file, std::ios::binary);
                out << part.doc.ToJson(true);
                ok = bool(out);
                if (!ok) error = "cannot write " + file.string();
            }
            else {
                Cad::FileFormat f;
                if (!Cad::FormatFromPath(file.string(), f)) { if (status) *status = "unknown format '" + format + "'"; return false; }
                ok = part.doc.ExportResult(file.string(), f, &error);
            }
            if (ok) { ProjectAssets().Refresh(); if (path) *path = file.generic_string(); }
            if (status) *status = ok ? "exported " + file.generic_string() : error;
            return ok;
        }
    }

    // ===================================================================== public API
    Entity CreateCadPart(GameContext& ctx, const std::string& name, const std::string& documentJson, std::string* status) {
        Scene* scene = ctx.CurrentScene();
        if (!scene) { if (status) *status = "no scene"; return {}; }
        if (CurrentMode() == Mode::Play) { if (status) *status = "stop the simulation first"; return {}; }
        Cad::Document doc;
        std::string error;
        if (!documentJson.empty() && !Cad::Document::FromJson(documentJson, doc, &error)) { if (status) *status = error; return {}; }
        const bool recorded = BeginEdit(ctx, "Create CAD part '" + name + "'");
        Entity e = scene->CreateEntity(name.empty() ? "CAD part" : name);
        e.LocalTransform().position = ctx.camera.GetOrbitTarget();
        e.Add<MeshRenderer>(MeshRenderer{ "Cube", "" });
        CadModel cad;
        cad.document = doc.ToJson();
        e.Add<CadModel>(cad);
        if (recorded) EndEdit(ctx); else Internal::NoteEdit();
        Select(e.Id() + 1);
        if (status) *status = "created " + e.GetName();
        return e;
    }

    bool SetCadDocument(GameContext& ctx, Entity e, const std::string& documentJson, std::string* status) {
        if (!e) { if (status) *status = "no entity"; return false; }
        Cad::Document doc;
        std::string error;
        if (!Cad::Document::FromJson(documentJson, doc, &error)) { if (status) *status = error; return false; }
        if (CurrentMode() == Mode::Play) { if (status) *status = "stop the simulation first"; return false; }
        const bool recorded = BeginEdit(ctx, "Edit CAD part '" + e.GetName() + "'");
        CadModel cad = e.Has<CadModel>() ? e.Get<CadModel>() : CadModel{};
        cad.document = doc.ToJson();
        ++cad.revision;
        if (!e.Has<MeshRenderer>()) e.Add<MeshRenderer>(MeshRenderer{ "Cube", "" });
        e.Add<CadModel>(cad);
        if (recorded) EndEdit(ctx); else Internal::NoteEdit();
        if (status) *status = "updated";
        return true;
    }

    std::string CadPartReport(Entity e) {
        if (!e || !e.Has<CadModel>()) return {};
        CadModel& cad = e.Get<CadModel>();
        Part& part = g_parts[e.GetGuid()];
        if (part.revision != cad.revision || cad.builtRevision != cad.revision) Build(e, cad, part);
        std::ostringstream o;
        auto str = [](const std::string& s) {
            std::string out = "\"";
            for (char c : s) { if (c == '"' || c == '\\') out += '\\'; if (static_cast<unsigned char>(c) < 0x20) { out += ' '; continue; } out += c; }
            return out + "\"";
        };
        auto num = [](double v) { char t[40]; std::snprintf(t, sizeof(t), "%.10g", std::isfinite(v) ? v : 0.0); return std::string(t); };
        const Cad::Properties& p = part.properties;
        o << "{\"kernel\":" << str(Cad::Kernel::Name()) << ",\"unit\":" << str(Cad::UnitSymbol(part.doc.unit))
          << ",\"ok\":" << (part.loadError.empty() && part.result.ok ? "true" : "false")
          << ",\"error\":" << str(!part.loadError.empty() ? part.loadError : part.result.firstError)
          << ",\"buildMilliseconds\":" << num(part.buildMs) << ",\"meshTriangles\":" << part.mesh.indices.size() / 3
          << ",\"parameters\":[";
        bool first = true;
        for (const Cad::Parameter& prm : part.doc.Parameters()) {
            o << (first ? "" : ",") << "{\"name\":" << str(prm.name) << ",\"expression\":" << str(prm.expression) << ",\"value\":" << num(prm.value) << ",\"error\":" << str(prm.error) << "}";
            first = false;
        }
        o << "],\"features\":[";
        first = true;
        for (const Cad::Feature& f : part.doc.Features()) {
            const Cad::FeatureState* st = part.doc.State(f.id);
            o << (first ? "" : ",") << "{\"id\":" << f.id << ",\"type\":" << str(Cad::ToString(f.type)) << ",\"name\":" << str(f.name)
              << ",\"suppressed\":" << (f.suppressed ? "true" : "false") << ",\"built\":" << (st && st->built ? "true" : "false")
              << ",\"skipped\":" << (st && st->skipped ? "true" : "false") << ",\"error\":" << str(st ? st->error : std::string());
            if (f.type == Cad::FeatureType::Sketch && st) o << ",\"degreesOfFreedom\":" << st->solve.degreesOfFreedom << ",\"solve\":" << str(st->solve.message);
            o << "}";
            first = false;
        }
        o << "],\"bodies\":" << part.doc.Bodies().size()
          << ",\"volume\":" << num(p.volume) << ",\"area\":" << num(p.area)
          << ",\"boundsMin\":[" << num(p.boundsMin.x) << "," << num(p.boundsMin.y) << "," << num(p.boundsMin.z) << "]"
          << ",\"boundsMax\":[" << num(p.boundsMax.x) << "," << num(p.boundsMax.y) << "," << num(p.boundsMax.z) << "]"
          << ",\"solids\":" << p.solids << ",\"faces\":" << p.faces << ",\"edges\":" << p.edges << ",\"valid\":" << (p.valid ? "true" : "false") << "}";
        return o.str();
    }

    bool UiItemRect(const std::string& id, glm::vec4& rect) {
        auto it = g_items.find(id);
        if (it == g_items.end()) return false;
        rect = it->second;
        return true;
    }

    bool ExportCadPart(Entity e, const std::string& format, std::string* path, std::string* status) {
        if (!e || !e.Has<CadModel>()) { if (status) *status = "not a CAD part"; return false; }
        CadModel& cad = e.Get<CadModel>();
        Part& part = g_parts[e.GetGuid()];
        if (part.revision != cad.revision || cad.builtRevision != cad.revision) Build(e, cad, part);
        if (format != "step" && format != "iges" && format != "stl" && format != "brep" && format != "phcad") { if (status) *status = "format must be step, iges, stl, brep or phcad"; return false; }
        return ExportPart(e, part, format, path, status);
    }

    namespace Internal {
        void CadReset() {
            for (const auto& [guid, part] : g_parts) Render::ReleaseMesh(CadModel::MeshName(guid));
            g_parts.clear();
            g_partGuid = 0;
            g_feature = 0;
            ResetTools();
        }

        void CadSync(GameContext& ctx, Scene& scene) {
            g_items.swap(g_itemsDrawn);   // the controls drawn last frame
            g_itemsDrawn.clear();
            // Millimetre parts need the orbit camera to come closer than its 0.5 m default.
            if (!g_parts.empty()) ctx.camera.Settings().orbitMinDistance = std::min(ctx.camera.Settings().orbitMinDistance, 0.01f);
            scene.Each<CadModel>([&](Entity e, CadModel& cad) {
                if (scene.IsPendingDestroy(e)) return;
                Part& part = g_parts[e.GetGuid()];
                if (cad.builtRevision != cad.revision || part.revision != cad.revision || (part.hasMesh && !cad.meshReady)) Build(e, cad, part);
                else if (part.hasMesh && !Render::HasMesh(CadModel::MeshName(e.GetGuid()))) Build(e, cad, part);   // renderer restarted
            });
            for (auto it = g_parts.begin(); it != g_parts.end();) {   // parts whose entity (or component) is gone
                Entity e = scene.FindByGuid(it->first);
                if (e && e.Has<CadModel>() && !scene.IsPendingDestroy(e)) { ++it; continue; }
                Render::ReleaseMesh(CadModel::MeshName(it->first));
                if (g_partGuid == it->first) { g_partGuid = 0; ResetTools(); }
                it = g_parts.erase(it);
            }
        }

        bool CadBlocksGizmo() { return g_partGuid != 0 && (g_sketching || g_pick != Pick::None); }

        void CadInspector(GameContext&, Scene&, Entity e) {
            Part* part = PartOf(e);
            if (!part) return;
            if (g_partGuid != e.GetGuid()) { g_partGuid = e.GetGuid(); g_feature = 0; ResetTools(); g_status.clear(); }
            if (!ImGui::CollapsingHeader("CAD part", ImGuiTreeNodeFlags_DefaultOpen)) return;
            const bool playing = CurrentMode() == Mode::Play;
            ImGui::TextDisabled("kernel: %s", Cad::Kernel::Name().c_str());
            if (!Cad::Kernel::Available()) ImGui::TextColored(Amber(), "built without a geometry kernel: sketches only");
            if (!part->loadError.empty()) { ImGui::TextColored(ImVec4(1, 0.4f, 0.35f, 1), "%s", part->loadError.c_str()); return; }
            ImGui::TextColored(part->result.ok ? Phosphor() : Amber(), "%s  (%d built, %d failed, %d skipped, %.1f ms)", part->result.ok ? "BUILT" : "ERRORS",
                               part->result.built, part->result.failed, part->result.skipped, part->buildMs);
            if (playing) { ImGui::TextDisabled("editing is disabled while the simulation plays"); ImGui::BeginDisabled(); }
            Cad::Document& doc = part->doc;
            // unit
            const Cad::Unit units[] = { Cad::Unit::Millimetre, Cad::Unit::Centimetre, Cad::Unit::Metre, Cad::Unit::Inch };
            ImGui::SetNextItemWidth(90.0f * GetLayout().scale);
            if (ImGui::BeginCombo("unit (lengths are not converted)", Cad::UnitSymbol(doc.unit))) {
                for (Cad::Unit u : units) if (ImGui::Selectable(Cad::UnitSymbol(u), doc.unit == u)) { doc.unit = u; Commit(e, doc); }
                ImGui::EndCombo();
            }
            // parameters
            if (ImGui::TreeNodeEx("Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
                std::string removeName;
                for (const Cad::Parameter& p : doc.Parameters()) {
                    ImGui::PushID(p.name.c_str());
                    if (ImGui::SmallButton("x")) removeName = p.name;
                    ImGui::SameLine();
                    ImGui::TextUnformatted(p.name.c_str());
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.35f);
                    std::string expr = p.expression;
                    if (ExprInput("##expr", expr, ImGui::GetContentRegionAvail().x * 0.6f)) {
                        std::string err;
                        if (doc.SetParameter(p.name, expr, &err)) Commit(e, doc); else g_status = err;
                    }
                    ImGui::SameLine();
                    if (p.error.empty()) ImGui::TextDisabled("= %.6g", p.value); else ImGui::TextColored(Amber(), "!");
                    ImGui::PopID();
                }
                if (!removeName.empty()) { std::string err; if (doc.RemoveParameter(removeName, &err)) Commit(e, doc); else g_status = "cannot remove: " + err; }
                char nb[64], eb[128];
                std::snprintf(nb, sizeof(nb), "%s", g_newParam.c_str());
                std::snprintf(eb, sizeof(eb), "%s", g_newParamExpr.c_str());
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.35f);
                if (ImGui::InputTextWithHint("##pn", "name", nb, sizeof(nb))) g_newParam = nb;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
                if (ImGui::InputText("##pe", eb, sizeof(eb))) g_newParamExpr = eb;
                ImGui::SameLine();
                if (ImGui::SmallButton("+ add")) {
                    std::string err;
                    if (doc.SetParameter(g_newParam, g_newParamExpr, &err)) { Commit(e, doc); g_newParam.clear(); } else g_status = err;
                }
                ImGui::TreePop();
            }
            // feature history
            if (ImGui::TreeNodeEx("Features", ImGuiTreeNodeFlags_DefaultOpen)) {
                const auto& features = doc.Features();
                for (size_t i = 0; i < features.size(); ++i) {
                    const Cad::Feature& f = features[i];
                    const Cad::FeatureState* st = doc.State(f.id);
                    if (doc.rollback == i) ImGui::TextColored(Amber(), "---- rolled back here ----");
                    ImGui::PushID(int(f.id));
                    const bool rolled = i >= doc.rollback;
                    ImVec4 colour = f.suppressed || rolled ? Muted() : st && !st->error.empty() ? ImVec4(1, 0.4f, 0.35f, 1) : st && st->skipped ? Amber() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
                    ImGui::PushStyleColor(ImGuiCol_Text, colour);
                    std::string label = std::string(FeatureIcon(f.type)) + " " + f.name + (f.suppressed ? "  (suppressed)" : "") + (st && !st->error.empty() ? "  (error)" : "");
                    const bool picked = ImGui::Selectable(label.c_str(), g_feature == f.id);
                    Track("feature:" + f.name);
                    if (picked) { if (g_feature != f.id) ResetTools(); g_feature = f.id; }
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                if (doc.rollback >= features.size() && doc.rollback != size_t(-1)) ImGui::TextColored(Amber(), "---- rolled back here ----");
                ImGui::SetNextItemWidth(-1.0f);
                AddFeatureMenu(e, *part);
                if (const Cad::Feature* sel = doc.FindFeature(g_feature)) {
                    const size_t index = doc.IndexOf(sel->id);
                    const float bw = (ImGui::GetContentRegionAvail().x - 3 * ImGui::GetStyle().ItemSpacing.x) / 4.0f;
                    if (ImGui::Button("up", ImVec2(bw, 0)) && index > 0) { std::string err; if (doc.MoveFeature(sel->id, index - 1, &err)) Commit(e, doc); else g_status = err; }
                    ImGui::SameLine();
                    if (ImGui::Button("down", ImVec2(bw, 0)) && index + 1 < features.size()) { std::string err; if (doc.MoveFeature(sel->id, index + 1, &err)) Commit(e, doc); else g_status = err; }
                    ImGui::SameLine();
                    if (ImGui::Button(sel->suppressed ? "unsuppress" : "suppress", ImVec2(bw, 0))) { Cad::Feature f = *sel; f.suppressed = !f.suppressed; if (doc.UpdateFeature(f)) Commit(e, doc); }
                    ImGui::SameLine();
                    if (ImGui::Button("delete", ImVec2(bw, 0))) {
                        std::string err;
                        const Cad::Id id = sel->id;
                        if (doc.RemoveFeature(id, &err)) { ResetTools(); g_feature = 0; Commit(e, doc); } else g_status = "cannot delete: " + err;
                    }
                    if (ImGui::Button(doc.rollback == index + 1 ? "roll forward" : "roll back to here", ImVec2(-1, 0))) {
                        doc.rollback = doc.rollback == index + 1 ? size_t(-1) : index + 1;
                        Commit(e, doc);
                    }
                }
                else if (doc.rollback != size_t(-1) && ImGui::Button("roll forward", ImVec2(-1, 0))) { doc.rollback = size_t(-1); Commit(e, doc); }
                ImGui::TreePop();
            }
            if (doc.FindFeature(g_feature)) {
                ImGui::SeparatorText("feature");
                FeatureEditor(e, *part);
            }
            ImGui::SeparatorText("inspect");
            Inspection(*part);
            ImGui::SeparatorText("export to project assets/exports");
            const char* formats[] = { "step", "iges", "stl", "brep", "phcad" };
            const float fw = (ImGui::GetContentRegionAvail().x - 4 * ImGui::GetStyle().ItemSpacing.x) / 5.0f;
            for (int i = 0; i < 5; ++i) {
                if (i) ImGui::SameLine();
                if (ImGui::Button(formats[i], ImVec2(fw, 0))) ExportPart(e, *part, formats[i], nullptr, &g_status);
            }
            if (playing) ImGui::EndDisabled();
            if (!g_status.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, Amber()); ImGui::TextWrapped("%s", g_status.c_str()); ImGui::PopStyleColor(); }
        }

        // ================================================================ viewport tools
        void CadViewport(GameContext&, Scene& scene) {
            if (!g_partGuid) return;
            Entity e = scene.FindByGuid(g_partGuid);
            Part* part = PartOf(e);
            if (!part || Selected(scene) != e || CurrentMode() == Mode::Play) { if (g_sketching || g_pick != Pick::None) ResetTools(); return; }
            const ViewInfo view = View(scene);
            if (!view.valid) return;
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            const glm::mat4 M = PartMatrix(e, part->doc);
            const glm::mat4 Minv = glm::inverse(M);
            const bool mouseFree = Internal::MouseInViewport() && !UI::WantsMouse();
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const float s = GetLayout().scale;

            // -------- picking faces / edges (fillet edges, measurement)
            if (g_pick != Pick::None && g_pickShape) {
                Cad::Feature* pf = g_pick == Pick::Edges ? part->doc.FindFeature(g_pickFeature) : nullptr;
                if (g_pick == Pick::Edges && !pf) { ResetTools(); return; }
                // edge under the cursor (screen space), else face (ray / triangle)
                int hoverEdge = 0, hoverFace = 0;
                float bestPx = 7.0f * s;
                for (const Cad::Tessellation::Polyline& line : g_pickMesh.edges) {
                    ImVec2 prev;
                    bool havePrev = false;
                    for (const glm::vec3& p : line.points) {
                        ImVec2 sp;
                        const bool ok = ToScreen(view, glm::vec3(M * glm::vec4(p, 1.0f)), sp);
                        if (ok && havePrev) { const float d = SegmentDistance(mouse, prev, sp); if (d < bestPx) { bestPx = d; hoverEdge = line.edge; } }
                        prev = sp; havePrev = ok;
                    }
                }
                glm::vec3 ro, rd;
                MouseRay(view, ro, rd);
                const glm::vec3 lo = glm::vec3(Minv * glm::vec4(ro, 1.0f)), ld = glm::normalize(glm::vec3(Minv * glm::vec4(rd, 0.0f)));
                float bestT = std::numeric_limits<float>::max();
                glm::vec3 hitPoint(0.0f);
                if (!hoverEdge) {
                    for (size_t t = 0; t + 2 < g_pickMesh.indices.size(); t += 3) {   // Moller-Trumbore
                        const glm::vec3 a = g_pickMesh.positions[g_pickMesh.indices[t]], b = g_pickMesh.positions[g_pickMesh.indices[t + 1]], c = g_pickMesh.positions[g_pickMesh.indices[t + 2]];
                        const glm::vec3 e1 = b - a, e2 = c - a, pv = glm::cross(ld, e2);
                        const float det = glm::dot(e1, pv);
                        if (std::fabs(det) < 1e-12f) continue;
                        const float inv = 1.0f / det;
                        const glm::vec3 tv = lo - a;
                        const float u = glm::dot(tv, pv) * inv;
                        if (u < 0 || u > 1) continue;
                        const glm::vec3 qv = glm::cross(tv, e1);
                        const float v = glm::dot(ld, qv) * inv;
                        if (v < 0 || u + v > 1) continue;
                        const float dist = glm::dot(e2, qv) * inv;
                        if (dist > 0 && dist < bestT) { bestT = dist; hoverFace = g_pickMesh.faceOf[t / 3]; hitPoint = lo + ld * dist; }
                    }
                }
                // draw edges: picked (fillet selection / measure) bright, hovered highlighted
                std::vector<int> chosen;
                if (pf) Cad::Document::ResolveEdges(g_pickShape, pf->edges, chosen, nullptr);
                for (int i = 0; i < g_measureCount; ++i) if (g_measure[i].edge) chosen.push_back(g_measure[i].edge);
                for (const Cad::Tessellation::Polyline& line : g_pickMesh.edges) {
                    const bool isChosen = std::find(chosen.begin(), chosen.end(), line.edge) != chosen.end();
                    const ImU32 col = line.edge == hoverEdge ? IM_COL32(255, 255, 255, 255) : isChosen ? IM_COL32(0x4C, 0xFF, 0x8A, 255) : IM_COL32(0xFF, 0xBE, 0x0B, 140);
                    const float width = (line.edge == hoverEdge || isChosen ? 3.0f : 1.2f) * s;
                    ImVec2 prev;
                    bool havePrev = false;
                    for (const glm::vec3& p : line.points) {
                        ImVec2 sp;
                        const bool ok = ToScreen(view, glm::vec3(M * glm::vec4(p, 1.0f)), sp);
                        if (ok && havePrev) dl->AddLine(prev, sp, col, width);
                        prev = sp; havePrev = ok;
                    }
                }
                if (hoverFace && !hoverEdge) {   // outline the hovered face's triangles
                    for (size_t t = 0; t + 2 < g_pickMesh.indices.size(); t += 3) {
                        if (g_pickMesh.faceOf[t / 3] != hoverFace) continue;
                        ImVec2 p[3];
                        bool ok = true;
                        for (int k = 0; k < 3; ++k) ok = ok && ToScreen(view, glm::vec3(M * glm::vec4(g_pickMesh.positions[g_pickMesh.indices[t + size_t(k)]], 1.0f)), p[k]);
                        if (ok) dl->AddTriangleFilled(p[0], p[1], p[2], IM_COL32(0x4C, 0xA8, 0xFF, 50));
                    }
                }
                if (mouseFree && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    if (g_pick == Pick::Edges && hoverEdge) {
                        Cad::Feature f = *pf;
                        if (f.edges.empty()) chosen.clear();
                        auto it = std::find(chosen.begin(), chosen.end(), hoverEdge);
                        if (it != chosen.end()) chosen.erase(it); else chosen.push_back(hoverEdge);
                        f.edges.clear();
                        for (int idx : chosen) f.edges.push_back(Cad::Document::MakeEdgeRef(g_pickShape, idx));
                        if (part->doc.UpdateFeature(f)) Commit(e, part->doc);
                    }
                    if (g_pick == Pick::Measure && (hoverEdge || hoverFace)) {
                        if (g_measureCount == 2) g_measureCount = 0;
                        g_measure[g_measureCount++] = { hoverEdge ? 0 : hoverFace, hoverEdge, glm::dvec3(hitPoint) };
                    }
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ResetTools();
                return;
            }
            if (!g_sketching) return;

            // -------- sketch editor
            Cad::Feature* live = part->doc.FindFeature(g_feature);
            if (!live || live->type != Cad::FeatureType::Sketch) { ResetTools(); return; }
            Cad::Feature f = *live;
            Cad::Sketch& sk = f.sketch;
            PrepareSketch(part->doc, sk);
            const Cad::Plane plane = SketchPlane(part->doc, f);
            auto toWorld = [&](glm::dvec2 p) { return glm::vec3(M * glm::vec4(glm::vec3(plane.ToWorld(p)), 1.0f)); };
            auto screenOf = [&](glm::dvec2 p, ImVec2& out) { return ToScreen(view, toWorld(p), out); };
            // mouse on the plane (sketch coordinates)
            glm::vec3 ro, rd;
            MouseRay(view, ro, rd);
            const glm::vec3 lo = glm::vec3(Minv * glm::vec4(ro, 1.0f)), ld = glm::vec3(Minv * glm::vec4(rd, 0.0f));
            const glm::dvec3 n = plane.Normal();
            const double denom = glm::dot(glm::dvec3(ld), n);
            bool onPlane = std::fabs(denom) > 1e-9;
            glm::dvec2 cursor(0.0);
            if (onPlane) {
                const double t = glm::dot(plane.origin - glm::dvec3(lo), n) / denom;
                const glm::dvec3 hit = glm::dvec3(lo) + glm::dvec3(ld) * t;
                cursor = { glm::dot(hit - plane.origin, plane.u), glm::dot(hit - plane.origin, plane.v) };
                onPlane = t > 0;
            }
            // pixel size in sketch units at the cursor (for snapping radii)
            ImVec2 c0, c1;
            double unitsPerPixel = 1.0;
            if (screenOf(cursor, c0) && screenOf(cursor + glm::dvec2(1.0, 0.0), c1)) unitsPerPixel = 1.0 / std::max(1e-6, double(std::hypot(c1.x - c0.x, c1.y - c0.y)));
            const double snapRadius = 9.0 * s * unitsPerPixel;
            Cad::Id hoverPoint = sk.NearestPoint(cursor.x, cursor.y, snapRadius, g_dragPoint);
            glm::dvec2 snapped = cursor;
            if (hoverPoint) snapped = sk.PointPosition(hoverPoint);
            else if (g_gridSnap && g_grid > 0) snapped = glm::round(cursor / g_grid) * g_grid;
            // hovered curve (screen distance)
            Cad::Id hoverCurve = 0;
            if (!hoverPoint) {
                float best = 7.0f * s;
                for (const Cad::Geometry& g : sk.Geometries()) {
                    if (g.type == Cad::GeometryType::Line) {
                        ImVec2 a, b;
                        if (screenOf(sk.PointPosition(g.a), a) && screenOf(sk.PointPosition(g.b), b)) { const float d = SegmentDistance(mouse, a, b); if (d < best) { best = d; hoverCurve = g.id; } }
                    }
                    else if (g.type == Cad::GeometryType::Circle || g.type == Cad::GeometryType::Arc) {
                        const double r = sk.RadiusOf(g.id);
                        const double d = std::fabs(glm::length(cursor - sk.PointPosition(g.a)) - r) / unitsPerPixel;
                        if (d < best) { best = float(d); hoverCurve = g.id; }
                    }
                }
            }

            // grid near the plane origin + axes
            {
                const double extent = std::max(50.0 * g_grid, glm::length(part->properties.boundsMax - part->properties.boundsMin));
                const int lines = 20;
                const double step = std::max(g_grid, extent / lines);
                for (int i = -lines; i <= lines; ++i) {
                    ImVec2 a, b;
                    if (screenOf({ i * step, -lines * step }, a) && screenOf({ i * step, lines * step }, b)) dl->AddLine(a, b, i == 0 ? IM_COL32(0xFF, 0x50, 0x50, 120) : IM_COL32(0x60, 0x66, 0x80, 40), 1.0f);
                    if (screenOf({ -lines * step, i * step }, a) && screenOf({ lines * step, i * step }, b)) dl->AddLine(a, b, i == 0 ? IM_COL32(0x50, 0xFF, 0x50, 120) : IM_COL32(0x60, 0x66, 0x80, 40), 1.0f);
                }
            }
            // geometry
            auto selected = [&](Cad::Id id) { return std::find(g_selection.begin(), g_selection.end(), id) != g_selection.end(); };
            for (const Cad::Geometry& g : sk.Geometries()) {
                const bool sel = selected(g.id), hov = g.id == hoverCurve;
                const ImU32 col = sel ? IM_COL32(0x4C, 0xFF, 0x8A, 255) : hov ? IM_COL32(255, 255, 255, 255) : g.construction ? IM_COL32(0x8A, 0x90, 0xA8, 200) : IM_COL32(0xFF, 0xBE, 0x0B, 255);
                const float width = (sel || hov ? 2.6f : 1.8f) * s;
                if (g.type == Cad::GeometryType::Line) {
                    ImVec2 a, b;
                    if (screenOf(sk.PointPosition(g.a), a) && screenOf(sk.PointPosition(g.b), b)) dl->AddLine(a, b, col, width);
                }
                else if (g.type == Cad::GeometryType::Circle || g.type == Cad::GeometryType::Arc) {
                    const glm::dvec2 c = sk.PointPosition(g.a);
                    const double r = sk.RadiusOf(g.id);
                    double a0 = 0, sweep = 2 * PI;
                    if (g.type == Cad::GeometryType::Arc) {
                        const glm::dvec2 p0 = sk.PointPosition(g.b), p1 = sk.PointPosition(g.c);
                        a0 = std::atan2(p0.y - c.y, p0.x - c.x);
                        sweep = std::atan2(p1.y - c.y, p1.x - c.x) - a0;
                        while (sweep <= 0) sweep += 2 * PI;
                    }
                    ImVec2 prev;
                    bool havePrev = false;
                    for (int k = 0; k <= 64; ++k) {
                        const double a = a0 + sweep * k / 64.0;
                        ImVec2 p;
                        const bool ok = screenOf(c + r * glm::dvec2(std::cos(a), std::sin(a)), p);
                        if (ok && havePrev) dl->AddLine(prev, p, col, width);
                        prev = p; havePrev = ok;
                    }
                }
            }
            for (const Cad::Geometry& g : sk.Geometries()) {
                if (g.type != Cad::GeometryType::Point) continue;
                ImVec2 p;
                if (!screenOf({ g.x, g.y }, p)) continue;
                const bool sel = selected(g.id), hov = g.id == hoverPoint;
                dl->AddCircleFilled(p, (hov || sel ? 5.0f : 3.2f) * s, sel ? IM_COL32(0x4C, 0xFF, 0x8A, 255) : hov ? IM_COL32(255, 255, 255, 255) : g.fixed ? IM_COL32(0xFF, 0x50, 0x50, 255) : IM_COL32(0xFF, 0xE0, 0x90, 255));
            }
            // dimension labels
            for (const Cad::Constraint& c : sk.Constraints()) {
                if (!Cad::IsDimension(c.type)) continue;
                glm::dvec2 at(0.0);
                const Cad::Geometry* ga = sk.Find(c.a);
                if (!ga) continue;
                if (ga->type == Cad::GeometryType::Line) at = 0.5 * (sk.PointPosition(ga->a) + sk.PointPosition(ga->b));
                else if (ga->type == Cad::GeometryType::Point && c.b && sk.Find(c.b) && sk.Find(c.b)->type == Cad::GeometryType::Point) at = 0.5 * (sk.PointPosition(c.a) + sk.PointPosition(c.b));
                else if (ga->type == Cad::GeometryType::Point) at = sk.PointPosition(c.a);
                else at = sk.PointPosition(ga->a) + glm::dvec2(sk.RadiusOf(ga->id), 0.0);
                ImVec2 p;
                if (!screenOf(at, p)) continue;
                char t[96];
                if (!c.expression.empty()) std::snprintf(t, sizeof(t), "%s=%.4g", c.expression.c_str(), c.value);
                else std::snprintf(t, sizeof(t), c.type == Cad::ConstraintType::Angle ? "%.4g deg" : c.type == Cad::ConstraintType::Diameter ? "D%.4g" : c.type == Cad::ConstraintType::Radius ? "R%.4g" : "%.4g", c.value);
                const ImVec2 ts = ImGui::CalcTextSize(t);
                dl->AddRectFilled(ImVec2(p.x + 4, p.y - ts.y - 4), ImVec2(p.x + 8 + ts.x, p.y), IM_COL32(0x12, 0x13, 0x1A, 0xD0));
                dl->AddText(ImVec2(p.x + 6, p.y - ts.y - 2), IM_COL32(0x4C, 0xFF, 0x8A, 255), t);
            }
            // pending tool preview
            if (onPlane && mouseFree) {
                ImVec2 sp;
                if (screenOf(snapped, sp)) dl->AddCircle(sp, 6.0f * s, IM_COL32(255, 255, 255, 160), 16, 1.0f);
                if (!g_clicks.empty() || g_chain) {
                    const glm::dvec2 first = g_chain ? sk.PointPosition(g_chain) : g_clicks[0];
                    ImVec2 a, b;
                    if (screenOf(first, a) && screenOf(snapped, b)) {
                        if (g_tool == Tool::Rectangle) {
                            ImVec2 c, d;
                            screenOf({ snapped.x, first.y }, c); screenOf({ first.x, snapped.y }, d);
                            dl->AddLine(a, c, IM_COL32(255, 255, 255, 160)); dl->AddLine(c, b, IM_COL32(255, 255, 255, 160));
                            dl->AddLine(b, d, IM_COL32(255, 255, 255, 160)); dl->AddLine(d, a, IM_COL32(255, 255, 255, 160));
                        }
                        else if (g_tool == Tool::Circle || (g_tool == Tool::Arc && g_clicks.size() == 1)) {
                            dl->AddLine(a, b, IM_COL32(255, 255, 255, 100));
                            const double r = glm::length(snapped - first);
                            ImVec2 prev; bool hp = false;
                            for (int k = 0; k <= 48; ++k) { ImVec2 p; const bool ok = screenOf(first + r * glm::dvec2(std::cos(2 * PI * k / 48), std::sin(2 * PI * k / 48)), p); if (ok && hp) dl->AddLine(prev, p, IM_COL32(255, 255, 255, 120)); prev = p; hp = ok; }
                        }
                        else dl->AddLine(a, b, IM_COL32(255, 255, 255, 160), 1.5f * s);
                    }
                }
            }

            // -------- input
            bool changed = false;
            auto pointAt = [&](glm::dvec2 p) -> Cad::Id { return hoverPoint ? hoverPoint : sk.AddPoint(p.x, p.y, g_construction); };
            auto autoAlign = [&](Cad::Id line) {   // nearly horizontal / vertical lines snap to it
                const Cad::Geometry* g = sk.Find(line);
                if (!g) return;
                const glm::dvec2 d = sk.PointPosition(g->b) - sk.PointPosition(g->a);
                const double angle = std::fabs(std::atan2(d.y, d.x)) * 180.0 / PI;
                if (angle < 3.0 || angle > 177.0) sk.AddConstraint(Cad::ConstraintType::Horizontal, line);
                else if (std::fabs(angle - 90.0) < 3.0) sk.AddConstraint(Cad::ConstraintType::Vertical, line);
            };
            if (mouseFree && onPlane) {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    switch (g_tool) {
                        case Tool::Select: {
                            const Cad::Id hit = hoverPoint ? hoverPoint : hoverCurve;
                            if (!ImGui::GetIO().KeyShift) g_selection.clear();
                            if (hit) {
                                auto it = std::find(g_selection.begin(), g_selection.end(), hit);
                                if (it == g_selection.end()) g_selection.push_back(hit); else g_selection.erase(it);
                            }
                            if (hoverPoint) { g_dragPoint = hoverPoint; SetInteractionHeld(true); }
                            break;
                        }
                        case Tool::Point: sk.AddPoint(snapped.x, snapped.y, g_construction); changed = true; break;
                        case Tool::Line: {
                            const Cad::Id p = pointAt(snapped);
                            if (g_chain && p != g_chain) { const Cad::Id l = sk.AddLine(g_chain, p, g_construction); if (l) autoAlign(l); changed = true; }
                            g_chain = p;
                            if (!changed) changed = !hoverPoint;   // a new start point
                            break;
                        }
                        case Tool::Rectangle:
                            if (g_clicks.empty()) g_clicks.push_back(snapped);
                            else {
                                if (sk.AddRectangle(g_clicks[0].x, g_clicks[0].y, snapped.x, snapped.y).empty()) g_status = "a rectangle needs a width and a height";
                                else changed = true;
                                g_clicks.clear();
                            }
                            break;
                        case Tool::Circle:
                            if (g_clicks.empty()) { g_clicks.push_back(snapped); g_chain = hoverPoint; }
                            else {
                                const double r = glm::length(snapped - g_clicks[0]);
                                const Cad::Id centre = g_chain ? g_chain : sk.AddPoint(g_clicks[0].x, g_clicks[0].y, true);
                                if (r > 0 && sk.AddCircle(centre, r, g_construction)) changed = true;
                                g_clicks.clear();
                                g_chain = 0;
                            }
                            break;
                        case Tool::Arc:
                            g_clicks.push_back(snapped);
                            if (g_clicks.size() == 3) {
                                const glm::dvec2 c = g_clicks[0];
                                const double r = glm::length(g_clicks[1] - c);
                                const glm::dvec2 dir = g_clicks[2] - c;
                                if (r > 0 && glm::length(dir) > 0) {
                                    const Cad::Id cp = sk.AddPoint(c.x, c.y, true);
                                    const Cad::Id p0 = sk.AddPoint(g_clicks[1].x, g_clicks[1].y);
                                    const glm::dvec2 e2 = c + glm::normalize(dir) * r;
                                    const Cad::Id p1 = sk.AddPoint(e2.x, e2.y);
                                    if (sk.AddArc(cp, p0, p1, g_construction)) changed = true;
                                }
                                g_clicks.clear();
                            }
                            break;
                    }
                }
                if (g_dragPoint && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    const glm::dvec2 target = g_gridSnap && g_grid > 0 ? glm::round(cursor / g_grid) * g_grid : cursor;
                    const glm::dvec2 before = sk.PointPosition(g_dragPoint);
                    if (glm::length(target - before) > 1e-9) {
                        // The dragged point is held where the cursor is while the rest solves; if the
                        // constraints do not allow that, it may move too; if nothing works, it stays.
                        const Cad::Sketch backup = sk;
                        sk.SetPoint(g_dragPoint, target.x, target.y);
                        Cad::Geometry* dragged = sk.Find(g_dragPoint);
                        const bool wasFixed = dragged->fixed;
                        dragged->fixed = true;
                        bool solved = !wasFixed && sk.Solve().status == Cad::SolveStatus::Solved;
                        if (Cad::Geometry* d = sk.Find(g_dragPoint)) d->fixed = wasFixed;
                        if (!solved) { sk = backup; sk.SetPoint(g_dragPoint, target.x, target.y); solved = sk.Solve().status == Cad::SolveStatus::Solved; }
                        if (solved) changed = true;
                        else sk = backup;
                    }
                }
            }
            if (g_dragPoint && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) { g_dragPoint = 0; SetInteractionHeld(false); }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && mouseFree) { g_chain = 0; g_clicks.clear(); }
            if (!ImGui::GetIO().WantTextInput) {
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { if (g_chain || !g_clicks.empty()) { g_chain = 0; g_clicks.clear(); } else if (g_tool != Tool::Select) g_tool = Tool::Select; else ResetTools(); }
                if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !g_selection.empty()) {
                    for (Cad::Id id : g_selection) sk.Remove(id);
                    g_selection.clear();
                    changed = true;
                }
            }

            // -------- tool window: a compact panel in the viewport's top-left corner, a few rows tall.
            // Its width is capped (and never more than the viewport), buttons wrap to it, sections are
            // labelled inline, and it never extends past the viewport, so the inspector and the other
            // panels stay uncovered and the lower part of the viewport stays free for sketching.
            const glm::vec4 vr = GetLayout().viewport;
            const float margin = 8.0f * s;
            const float width = std::max(std::min(380.0f * s, vr.z - 2.0f * margin), std::min(200.0f * s, vr.z - 2.0f * margin));
            const float maxHeight = std::max(120.0f * s, vr.w - 2.0f * margin);
            ImGui::SetNextWindowPos(ImVec2(vr.x + margin, vr.y + margin));
            ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, maxHeight));
            ImGui::SetNextWindowBgAlpha(0.88f);
            const ImGuiWindowFlags toolFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
                                             | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
            if (ImGui::Begin("##cadsketchtools", nullptr, toolFlags)) {
                g_itemsDrawn["window:sketch tools"] = glm::vec4(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, ImGui::GetWindowSize().x, ImGui::GetWindowSize().y);
                // places the next widget of `w` pixels on the current row if it fits, else on a new row
                bool rowStart = true;
                auto flow = [&](float w) {
                    if (!rowStart) {
                        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
                        if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w <= right) ImGui::SameLine();
                    }
                    rowStart = false;
                };
                auto buttonWidth = [](const char* label) { return ImGui::CalcTextSize(label).x + 2.0f * ImGui::GetStyle().FramePadding.x; };
                // a section starts a new row with its label; its buttons flow after it and wrap
                auto section = [&](const char* title) {
                    rowStart = true;
                    if (!*title) return;
                    ImGui::TextDisabled("%s", title);
                    rowStart = false;
                };

                // title row: sketch, unit, FINISH SKETCH (right-aligned); then the solver status
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(Amber(), "SKETCH %s (%s)", f.name.c_str(), Cad::UnitSymbol(part->doc.unit));
                const float finishW = buttonWidth("FINISH SKETCH");
                ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - finishW));
                const bool finish = ImGui::SmallButton("FINISH SKETCH");
                Track("FINISH SKETCH");
                const Cad::SolveResult& status = sk.LastSolve();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(status.status != Cad::SolveStatus::Solved ? ImVec4(1, 0.4f, 0.35f, 1) : status.degreesOfFreedom == 0 ? Phosphor() : Amber(), "%s", status.message.c_str());
                ImGui::PopTextWrapPos();

                section("draw");
                const char* tools[] = { "select", "point", "line", "rectangle", "circle", "arc" };
                for (int i = 0; i < 6; ++i) {
                    flow(buttonWidth(tools[i]));
                    const bool active = int(g_tool) == i;
                    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    const bool pressed = ImGui::SmallButton(tools[i]);
                    Track(std::string("tool:") + tools[i]);
                    if (pressed) { g_tool = Tool(i); g_clicks.clear(); g_chain = 0; }
                    if (active) ImGui::PopStyleColor();
                }
                const float box = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
                section("");
                flow(box + ImGui::CalcTextSize("construction").x);
                ImGui::Checkbox("construction", &g_construction);
                flow(box + ImGui::CalcTextSize("grid snap").x);
                ImGui::Checkbox("grid snap", &g_gridSnap);
                flow(60.0f * s);
                ImGui::SetNextItemWidth(60.0f * s);
                float grid = float(g_grid);
                if (ImGui::InputFloat("##grid", &grid, 0, 0, "%.3g")) g_grid = std::max(1e-4f, grid);

                // constraints for the selection
                std::vector<const Cad::Geometry*> sel;
                for (Cad::Id id : g_selection) if (const Cad::Geometry* g = sk.Find(id)) sel.push_back(g);
                auto count = [&](Cad::GeometryType t) { int n = 0; for (auto* g : sel) n += g->type == t; return n; };
                const int pts = count(Cad::GeometryType::Point), lns = count(Cad::GeometryType::Line);
                const int rnd = count(Cad::GeometryType::Circle) + count(Cad::GeometryType::Arc);
                const size_t k = sel.size();
                auto id = [&](Cad::GeometryType t, int nth) { int i = 0; for (auto* g : sel) if (g->type == t || (t == Cad::GeometryType::Circle && g->type == Cad::GeometryType::Arc)) { if (i++ == nth) return g->id; } return Cad::Id(0); };
                auto button = [&](const char* label, bool enabled, auto&& fn) {
                    flow(buttonWidth(label));
                    ImGui::BeginDisabled(!enabled);
                    const bool pressed = ImGui::SmallButton(label);
                    Track(std::string("constrain:") + label);
                    if (pressed) { fn(); changed = true; }
                    ImGui::EndDisabled();
                };
                const Cad::GeometryType P = Cad::GeometryType::Point, L = Cad::GeometryType::Line, C = Cad::GeometryType::Circle;
                section("constrain");
                button("coincident", k == 2 && pts == 2, [&] { sk.AddConstraint(Cad::ConstraintType::Coincident, id(P, 0), id(P, 1)); });
                button("horiz", (k == 1 && lns == 1) || (k == 2 && pts == 2), [&] { if (lns) sk.AddConstraint(Cad::ConstraintType::Horizontal, id(L, 0)); else sk.AddConstraint(Cad::ConstraintType::Horizontal, id(P, 0), id(P, 1)); });
                button("vert", (k == 1 && lns == 1) || (k == 2 && pts == 2), [&] { if (lns) sk.AddConstraint(Cad::ConstraintType::Vertical, id(L, 0)); else sk.AddConstraint(Cad::ConstraintType::Vertical, id(P, 0), id(P, 1)); });
                button("parallel", k == 2 && lns == 2, [&] { sk.AddConstraint(Cad::ConstraintType::Parallel, id(L, 0), id(L, 1)); });
                button("perp", k == 2 && lns == 2, [&] { sk.AddConstraint(Cad::ConstraintType::Perpendicular, id(L, 0), id(L, 1)); });
                button("tangent", k == 2 && rnd >= 1 && lns + rnd == 2, [&] { if (lns) sk.AddConstraint(Cad::ConstraintType::Tangent, id(L, 0), id(C, 0)); else sk.AddConstraint(Cad::ConstraintType::Tangent, id(C, 0), id(C, 1)); });
                button("equal", k == 2 && (lns == 2 || rnd == 2), [&] { sk.AddConstraint(Cad::ConstraintType::Equal, sel[0]->id, sel[1]->id); });
                button("concentric", k == 2 && rnd == 2, [&] { sk.AddConstraint(Cad::ConstraintType::Concentric, id(C, 0), id(C, 1)); });
                button("midpoint", k == 2 && pts == 1 && lns == 1, [&] { sk.AddConstraint(Cad::ConstraintType::Midpoint, id(P, 0), id(L, 0)); });
                button("on curve", k == 2 && pts == 1 && (lns == 1 || rnd == 1), [&] { if (lns) sk.AddConstraint(Cad::ConstraintType::PointOnLine, id(P, 0), id(L, 0)); else sk.AddConstraint(Cad::ConstraintType::PointOnCircle, id(P, 0), id(C, 0)); });
                button("symmetric", k == 3 && pts == 2 && lns == 1, [&] { sk.AddConstraint(Cad::ConstraintType::Symmetric, id(P, 0), id(P, 1), id(L, 0)); });
                button("fix", k >= 1 && pts == int(k), [&] { for (auto* g : sel) sk.AddConstraint(Cad::ConstraintType::Fix, g->id); });
                section("dimension");
                button("distance", k == 2 && pts == 2, [&] { sk.AddDimension(Cad::ConstraintType::Distance, id(P, 0), id(P, 1)); });
                button("horiz dist", k == 2 && pts == 2, [&] { sk.AddDimension(Cad::ConstraintType::HorizontalDistance, id(P, 0), id(P, 1)); });
                button("vert dist", k == 2 && pts == 2, [&] { sk.AddDimension(Cad::ConstraintType::VerticalDistance, id(P, 0), id(P, 1)); });
                button("length", k == 1 && lns == 1, [&] { sk.AddDimension(Cad::ConstraintType::Length, id(L, 0)); });
                button("point-line", k == 2 && pts == 1 && lns == 1, [&] { sk.AddDimension(Cad::ConstraintType::PointLineDistance, id(P, 0), id(L, 0)); });
                button("radius", k == 1 && rnd == 1, [&] { sk.AddDimension(Cad::ConstraintType::Radius, id(C, 0)); });
                button("diameter", k == 1 && rnd == 1, [&] { sk.AddDimension(Cad::ConstraintType::Diameter, id(C, 0)); });
                button("angle", k == 2 && lns == 2, [&] { sk.AddDimension(Cad::ConstraintType::Angle, id(L, 0), id(L, 1)); });
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("LMB tool  RMB orbit  ESC end  DEL delete  SHIFT multi-select");
                ImGui::PopTextWrapPos();
                if (finish) ResetTools();
            }
            ImGui::End();

            if (changed) {
                const Cad::SolveResult r = sk.Solve();
                if (r.status != Cad::SolveStatus::Solved) {
                    g_status = "not applied: " + r.message;   // the edit would over-constrain the sketch
                    return;
                }
                if (part->doc.UpdateFeature(f)) Commit(e, part->doc);
                // keep the selection valid
                g_selection.erase(std::remove_if(g_selection.begin(), g_selection.end(), [&](Cad::Id i) { return !f.sketch.Find(i); }), g_selection.end());
            }
        }

        std::string StarterCadDocument() {
            Cad::Document doc;
            doc.SetParameter("width", "40");
            doc.SetParameter("length", "25");
            doc.SetParameter("depth", "10");
            Cad::Feature sketch;
            sketch.type = Cad::FeatureType::Sketch;
            sketch.name = "Profile";
            const std::vector<Cad::Id> l = sketch.sketch.AddRectangle(0, 0, 40, 25);
            sketch.sketch.AddConstraint(Cad::ConstraintType::Fix, sketch.sketch.Find(l[0])->a);
            const Cad::Id w = sketch.sketch.AddDimension(Cad::ConstraintType::Length, l[0]);
            const Cad::Id h = sketch.sketch.AddDimension(Cad::ConstraintType::Length, l[1]);
            sketch.sketch.SetConstraintExpression(w, "width");
            sketch.sketch.SetConstraintExpression(h, "length");
            sketch.sketch.Solve();
            const Cad::Id sk = doc.AddFeature(sketch);
            Cad::Feature extrude;
            extrude.type = Cad::FeatureType::Extrude;
            extrude.profileSketch = sk;
            extrude.values["distance"] = "depth";
            doc.AddFeature(extrude);
            return doc.ToJson();
        }

        bool CadBounds(Entity e, glm::vec3& lo, glm::vec3& hi) {
            Part* part = PartOf(e);
            if (!part || !part->doc.Result()) return false;
            const glm::mat4 M = PartMatrix(e, part->doc);
            const glm::dvec3 a = part->properties.boundsMin, b = part->properties.boundsMax;
            lo = glm::vec3(std::numeric_limits<float>::max());
            hi = glm::vec3(-std::numeric_limits<float>::max());
            for (int i = 0; i < 8; ++i) {
                const glm::vec3 c(i & 1 ? b.x : a.x, i & 2 ? b.y : a.y, i & 4 ? b.z : a.z);
                const glm::vec3 w = glm::vec3(M * glm::vec4(c, 1.0f));
                lo = glm::min(lo, w);
                hi = glm::max(hi, w);
            }
            return true;
        }
    }
}
