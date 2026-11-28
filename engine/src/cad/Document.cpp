// Parametric CAD document (see Phisiks/Cad/Document.h).
#include "Phisiks/Cad/Document.h"
#include "Phisiks/Cad/Expression.h"

#include "CadJson.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <stdexcept>

namespace Phisiks::Cad {
    namespace {
        struct FeatureName { FeatureType type; const char* name; };
        const FeatureName FEATURE_NAMES[] = {
            { FeatureType::Sketch, "sketch" }, { FeatureType::Extrude, "extrude" }, { FeatureType::Revolve, "revolve" },
            { FeatureType::Box, "box" }, { FeatureType::Cylinder, "cylinder" }, { FeatureType::Sphere, "sphere" },
            { FeatureType::Fillet, "fillet" }, { FeatureType::Chamfer, "chamfer" }, { FeatureType::Boolean, "boolean" },
            { FeatureType::LinearPattern, "linearPattern" }, { FeatureType::CircularPattern, "circularPattern" },
            { FeatureType::Mirror, "mirror" }, { FeatureType::Import, "import" },
        };
        const char* DisplayName(FeatureType t) {
            switch (t) {
                case FeatureType::Sketch: return "Sketch"; case FeatureType::Extrude: return "Extrude"; case FeatureType::Revolve: return "Revolve";
                case FeatureType::Box: return "Box"; case FeatureType::Cylinder: return "Cylinder"; case FeatureType::Sphere: return "Sphere";
                case FeatureType::Fillet: return "Fillet"; case FeatureType::Chamfer: return "Chamfer"; case FeatureType::Boolean: return "Boolean";
                case FeatureType::LinearPattern: return "Linear pattern"; case FeatureType::CircularPattern: return "Circular pattern";
                case FeatureType::Mirror: return "Mirror"; case FeatureType::Import: return "Import";
            }
            return "Feature";
        }
        // default expressions per feature type
        const std::map<std::string, std::string>& Defaults(FeatureType t) {
            static const std::map<FeatureType, std::map<std::string, std::string>> d = {
                { FeatureType::Sketch, { { "offset", "0" } } },
                { FeatureType::Extrude, { { "distance", "10" } } },
                { FeatureType::Revolve, { { "angle", "360" } } },
                { FeatureType::Box, { { "x", "10" }, { "y", "10" }, { "z", "10" } } },
                { FeatureType::Cylinder, { { "radius", "5" }, { "height", "10" } } },
                { FeatureType::Sphere, { { "radius", "5" } } },
                { FeatureType::Fillet, { { "radius", "1" } } },
                { FeatureType::Chamfer, { { "distance", "1" } } },
                { FeatureType::LinearPattern, { { "count", "3" }, { "spacing", "10" } } },
                { FeatureType::CircularPattern, { { "count", "6" }, { "angle", "360" } } },
            };
            static const std::map<std::string, std::string> none;
            auto it = d.find(t);
            return it == d.end() ? none : it->second;
        }
        bool MakesBody(const Feature& f) {
            return f.type == FeatureType::Extrude || f.type == FeatureType::Revolve || f.type == FeatureType::Box || f.type == FeatureType::Cylinder
                || f.type == FeatureType::Sphere || f.type == FeatureType::Import;
        }
        bool ModifiesBody(const Feature& f) {
            return f.type == FeatureType::Fillet || f.type == FeatureType::Chamfer || f.type == FeatureType::Boolean || f.type == FeatureType::LinearPattern
                || f.type == FeatureType::CircularPattern || f.type == FeatureType::Mirror || (MakesBody(f) && f.op != FeatureOp::NewBody);
        }
        json V3(const glm::dvec3& v) { return json::array({ v.x, v.y, v.z }); }
        glm::dvec3 V3(const json& j, const glm::dvec3& fallback) {
            if (!j.is_array() || j.size() != 3) return fallback;
            glm::dvec3 v(j[0].get<double>(), j[1].get<double>(), j[2].get<double>());
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) throw std::runtime_error("non-finite vector");
            return v;
        }
        [[noreturn]] void Fail(const std::string& m) { throw std::runtime_error(m); }
    }

    const char* ToString(FeatureType type) {
        for (const FeatureName& f : FEATURE_NAMES) if (f.type == type) return f.name;
        return "?";
    }
    bool ParseFeatureType(const std::string& text, FeatureType& out) {
        for (const FeatureName& f : FEATURE_NAMES) if (text == f.name) { out = f.type; return true; }
        return false;
    }
    const char* ToString(FeatureOp op) {
        switch (op) {
            case FeatureOp::NewBody: return "new";
            case FeatureOp::Join: return "join";
            case FeatureOp::Cut: return "cut";
            case FeatureOp::Intersect: return "intersect";
        }
        return "?";
    }
    namespace {
        bool ParseOp(const std::string& s, FeatureOp& op) {
            for (FeatureOp o : { FeatureOp::NewBody, FeatureOp::Join, FeatureOp::Cut, FeatureOp::Intersect }) if (s == ToString(o)) { op = o; return true; }
            return false;
        }
        const char* BooleanName(BooleanOp b) { return b == BooleanOp::Union ? "union" : b == BooleanOp::Subtract ? "subtract" : "intersect"; }
        bool ParseBoolean(const std::string& s, BooleanOp& b) {
            if (s == "union") b = BooleanOp::Union; else if (s == "subtract") b = BooleanOp::Subtract; else if (s == "intersect") b = BooleanOp::Intersect; else return false;
            return true;
        }
    }

    // ======================================================================== parameters
    bool Document::ParameterValue(const std::string& name, double& value) const {
        for (const Parameter& p : m_parameters) if (p.name == name) { value = p.value; return p.error.empty(); }
        return false;
    }
    bool Document::SetParameter(const std::string& name, const std::string& expression, std::string* error) {
        if (!IsValidParameterName(name)) { if (error) *error = "'" + name + "' is not a valid parameter name (letters, digits, _; not a function name)"; return false; }
        if (expression.empty()) { if (error) *error = "empty expression"; return false; }
        const std::vector<Parameter> before = m_parameters;
        auto it = std::find_if(m_parameters.begin(), m_parameters.end(), [&](const Parameter& p) { return p.name == name; });
        if (it == m_parameters.end()) { m_parameters.push_back({ name, expression }); }
        else it->expression = expression;
        std::string e;
        if (!EvaluateParameters(&e)) { m_parameters = before; EvaluateParameters(nullptr); if (error) *error = e; return false; }
        return true;
    }
    bool Document::RemoveParameter(const std::string& name, std::string* error) {
        auto uses = [&](const std::string& expr) { for (const std::string& n : ExpressionNames(expr)) if (n == name) return true; return false; };
        for (const Parameter& p : m_parameters) if (p.name != name && uses(p.expression)) { if (error) *error = "parameter '" + p.name + "' uses it"; return false; }
        for (const Feature& f : m_features) {
            for (const auto& [k, v] : f.values) if (uses(v)) { if (error) *error = f.name + " uses it"; return false; }
            for (const Constraint& c : f.sketch.Constraints()) if (uses(c.expression)) { if (error) *error = f.name + " (a dimension) uses it"; return false; }
        }
        const size_t n = m_parameters.size();
        m_parameters.erase(std::remove_if(m_parameters.begin(), m_parameters.end(), [&](const Parameter& p) { return p.name == name; }), m_parameters.end());
        if (m_parameters.size() == n) { if (error) *error = "no parameter '" + name + "'"; return false; }
        return true;
    }
    bool Document::EvaluateParameters(std::string* error) {
        // Topological evaluation: repeat passes until nothing changes; leftovers are cycles or errors.
        for (Parameter& p : m_parameters) { p.error = "unresolved"; }
        bool progress = true;
        while (progress) {
            progress = false;
            for (Parameter& p : m_parameters) {
                if (p.error.empty()) continue;
                bool ready = true;
                for (const std::string& n : ExpressionNames(p.expression)) {
                    auto it = std::find_if(m_parameters.begin(), m_parameters.end(), [&](const Parameter& q) { return q.name == n; });
                    if (it == m_parameters.end()) { ready = true; break; }   // unknown name: let Evaluate report it
                    if (!it->error.empty()) ready = false;
                }
                if (!ready) continue;
                std::string e;
                double v = 0;
                if (Evaluate(p.expression, [&](const std::string& n, double& out) { return ParameterValue(n, out); }, v, &e)) { p.value = v; p.error.clear(); }
                else p.error = e;
                if (p.error.empty()) progress = true;
                else if (p.error != "unresolved") progress = true;   // a definite error is final
            }
            bool remaining = false;
            for (const Parameter& p : m_parameters) if (p.error == "unresolved") remaining = true;
            if (!remaining) break;
        }
        bool ok = true;
        for (Parameter& p : m_parameters) {
            if (p.error == "unresolved") p.error = "circular reference";
            if (!p.error.empty()) { ok = false; if (error && error->empty()) *error = "parameter '" + p.name + "': " + p.error; }
        }
        return ok;
    }
    bool Document::Eval(const std::string& expression, double& out, std::string* error) const {
        return Evaluate(expression, [&](const std::string& n, double& v) { return ParameterValue(n, v); }, out, error);
    }

    // ======================================================================== features
    size_t Document::IndexOf(Id id) const {
        for (size_t i = 0; i < m_features.size(); ++i) if (m_features[i].id == id) return i;
        return size_t(-1);
    }
    const Feature* Document::FindFeature(Id id) const { const size_t i = IndexOf(id); return i == size_t(-1) ? nullptr : &m_features[i]; }
    Feature* Document::FindFeature(Id id) { const size_t i = IndexOf(id); return i == size_t(-1) ? nullptr : &m_features[i]; }

    std::vector<Id> Document::Dependencies(const Feature& f) const {
        std::vector<Id> d;
        const size_t index = IndexOf(f.id) == size_t(-1) ? m_features.size() : IndexOf(f.id);
        auto latestBody = [&](size_t before, Id exclude) -> Id {
            // the latest body-creating feature before `before` whose body still exists there (not consumed)
            std::set<Id> consumed;
            for (size_t i = 0; i < before && i < m_features.size(); ++i)
                if (m_features[i].type == FeatureType::Boolean && !m_features[i].suppressed) consumed.insert(m_features[i].tool);
            for (size_t i = std::min(before, m_features.size()); i-- > 0;) {
                const Feature& g = m_features[i];
                if (!g.suppressed && MakesBody(g) && g.op == FeatureOp::NewBody && g.id != exclude && !consumed.count(g.id)) return g.id;
            }
            return 0;
        };
        if (f.type == FeatureType::Extrude || f.type == FeatureType::Revolve) if (f.profileSketch) d.push_back(f.profileSketch);
        if (ModifiesBody(f)) {
            const Id body = f.body ? f.body : latestBody(index, f.type == FeatureType::Boolean ? f.tool : 0);
            if (body) d.push_back(body);
        }
        if (f.type == FeatureType::Boolean && f.tool) d.push_back(f.tool);
        if ((f.type == FeatureType::LinearPattern || f.type == FeatureType::CircularPattern) && f.source) d.push_back(f.source);
        return d;
    }
    std::vector<Id> Document::Dependents(Id id) const {
        std::vector<Id> out;
        for (const Feature& f : m_features) {
            if (f.id == id) continue;
            const std::vector<Id> d = Dependencies(f);
            if (std::find(d.begin(), d.end(), id) != d.end()) out.push_back(f.id);
        }
        return out;
    }

    Id Document::AddFeature(Feature f) { return InsertFeature(std::move(f), m_features.size()); }
    Id Document::InsertFeature(Feature f, size_t index) {
        if (index > m_features.size()) return 0;
        f.id = m_nextId++;
        for (const auto& [k, v] : Defaults(f.type)) if (!f.values.count(k)) f.values[k] = v;
        if (f.name.empty()) {
            int n = 1;
            for (const Feature& g : m_features) if (g.type == f.type) ++n;
            f.name = std::string(DisplayName(f.type)) + " " + std::to_string(n);
        }
        m_features.insert(m_features.begin() + long(index), f);
        // dependencies must come before it
        const std::vector<Id> deps = Dependencies(m_features[index]);
        for (Id d : deps) {
            const size_t di = IndexOf(d);
            if (di == size_t(-1) || di >= index) { m_features.erase(m_features.begin() + long(index)); --m_nextId; return 0; }
        }
        return f.id;
    }
    bool Document::UpdateFeature(const Feature& f, std::string* error) {
        Feature* cur = FindFeature(f.id);
        if (!cur) { if (error) *error = "no feature " + std::to_string(f.id); return false; }
        if (cur->type != f.type) { if (error) *error = "a feature's type cannot change"; return false; }
        const Feature before = *cur;
        *cur = f;
        const size_t index = IndexOf(f.id);
        for (Id d : Dependencies(*cur)) {
            const size_t di = IndexOf(d);
            if (di == size_t(-1) || di >= index) { *cur = before; if (error) *error = "it would use a feature that comes after it (or does not exist)"; return false; }
        }
        return true;
    }
    bool Document::RemoveFeature(Id id, std::string* error) {
        const size_t i = IndexOf(id);
        if (i == size_t(-1)) { if (error) *error = "no feature " + std::to_string(id); return false; }
        const std::vector<Id> deps = Dependents(id);
        if (!deps.empty()) {
            if (error) {
                *error = "used by ";
                for (size_t k = 0; k < deps.size(); ++k) *error += (k ? ", " : "") + FindFeature(deps[k])->name;
            }
            return false;
        }
        m_features.erase(m_features.begin() + long(i));
        m_state.erase(id);
        return true;
    }
    bool Document::MoveFeature(Id id, size_t newIndex, std::string* error) {
        const size_t i = IndexOf(id);
        if (i == size_t(-1) || newIndex >= m_features.size()) { if (error) *error = "invalid position"; return false; }
        const std::vector<Feature> before = m_features;
        Feature f = m_features[i];
        m_features.erase(m_features.begin() + long(i));
        m_features.insert(m_features.begin() + long(newIndex), f);
        for (size_t k = 0; k < m_features.size(); ++k)
            for (Id d : Dependencies(m_features[k])) {
                const size_t di = IndexOf(d);
                if (di == size_t(-1) || di >= k) { m_features = before; if (error) *error = "the move would put a feature before something it uses"; return false; }
            }
        return true;
    }

    const FeatureState* Document::State(Id id) const { auto it = m_state.find(id); return it == m_state.end() ? nullptr : &it->second; }
    const Body* Document::FindBody(Id id) const { for (const Body& b : m_bodies) if (b.id == id) return &b; return nullptr; }
    Shape Document::Result() const {
        std::vector<Shape> shapes;
        for (const Body& b : m_bodies) if (b.shape) shapes.push_back(b.shape);
        if (shapes.size() == 1) return shapes[0];
        return Kernel::Compound(shapes);
    }
    Shape Document::ShapeAfter(Id id) const { auto it = m_after.find(id); return it == m_after.end() ? Shape() : it->second; }
    Shape Document::ShapeBefore(Id id) const { auto it = m_before.find(id); return it == m_before.end() ? Shape() : it->second; }

    EdgeRef Document::MakeEdgeRef(const Shape& shape, int edgeIndex) {
        for (const EdgeInfo& e : Kernel::EdgesOf(shape)) if (e.index == edgeIndex) return { e.midpoint, e.tangent };
        return {};
    }
    bool Document::ResolveEdges(const Shape& shape, const std::vector<EdgeRef>& refs, std::vector<int>& indices, std::string* error) {
        indices.clear();
        if (refs.empty()) return true;
        const std::vector<EdgeInfo> edges = Kernel::EdgesOf(shape);
        const Properties p = Kernel::Measure(shape);
        const double diag = glm::length(p.boundsMax - p.boundsMin);
        const double tolerance = std::max(0.25 * diag, 1e-6);
        for (const EdgeRef& r : refs) {
            int best = 0;
            double bestD = tolerance;
            for (const EdgeInfo& e : edges) {
                const double align = glm::length(r.tangent) > 0 && glm::length(e.tangent) > 0 ? std::fabs(glm::dot(glm::normalize(r.tangent), e.tangent)) : 1.0;
                if (align < 0.95) continue;
                const double d = glm::length(e.midpoint - r.midpoint);
                if (d < bestD) { bestD = d; best = e.index; }
            }
            if (!best) { if (error) *error = "edge reference lost (no edge near the selected one after the model changed): reselect the edges"; return false; }
            if (std::find(indices.begin(), indices.end(), best) == indices.end()) indices.push_back(best);
        }
        return true;
    }

    // ======================================================================== regeneration
    RegenerateResult Document::Regenerate(const AssetResolver& resolver) {
        using Clock = std::chrono::steady_clock;
        const auto t0 = Clock::now();
        RegenerateResult result;
        m_state.clear(); m_after.clear(); m_before.clear(); m_tools.clear(); m_bodies.clear();
        std::string paramError;
        EvaluateParameters(&paramError);
        std::set<Id> failed;
        std::map<Id, Sketch> solved;
        std::map<Id, Plane> planes;
        std::map<Id, Id> bodyOf;                 // feature -> the body it created / modified
        auto bodyIndex = [&](Id id) -> int { for (size_t i = 0; i < m_bodies.size(); ++i) if (m_bodies[i].id == id) return int(i); return -1; };
        auto num = [&](const Feature& f, const char* key) {
            auto it = f.values.find(key);
            const std::string expr = it == f.values.end() ? Defaults(f.type).at(key) : it->second;
            double v = 0;
            std::string e;
            if (!Eval(expr, v, &e)) Fail(std::string(key) + " " + e);
            return v;
        };
        const size_t end = std::min(rollback, m_features.size());
        for (size_t index = 0; index < end; ++index) {
            const Feature& f = m_features[index];
            FeatureState& st = m_state[f.id];
            if (f.suppressed) continue;
            bool blocked = false;
            for (Id d : Dependencies(f)) if (failed.count(d)) blocked = true;
            if (blocked) { st.skipped = true; failed.insert(f.id); ++result.skipped; continue; }
            const auto ft = Clock::now();
            try {
                std::string err;
                auto check = [&](const Shape& s, const char* what) { if (!s) Fail(std::string(what) + " failed" + (err.empty() ? "" : ": " + err)); return s; };
                // target body for modifying features
                auto target = [&]() -> int {
                    const std::vector<Id> deps = Dependencies(f);
                    Id want = f.body;
                    if (!want) for (Id d : deps) if (d != f.profileSketch && d != f.tool && d != f.source) { want = d; break; }
                    const int bi = bodyIndex(want);
                    if (bi < 0) Fail("there is no body to modify (create one first, or choose the target body)");
                    return bi;
                };
                auto combine = [&](const Shape& tool, Id creator) {
                    m_tools[f.id] = tool;
                    if (f.op == FeatureOp::NewBody) {
                        Body b; b.id = creator; b.shape = tool; b.lastFeature = f.id;
                        b.name = "Body " + std::to_string(m_bodies.size() + 1);
                        m_bodies.push_back(b);
                        bodyOf[f.id] = creator;
                        return;
                    }
                    const int bi = target();
                    m_before[f.id] = m_bodies[size_t(bi)].shape;
                    const BooleanOp op = f.op == FeatureOp::Join ? BooleanOp::Union : f.op == FeatureOp::Cut ? BooleanOp::Subtract : BooleanOp::Intersect;
                    m_bodies[size_t(bi)].shape = check(Kernel::Boolean(m_bodies[size_t(bi)].shape, tool, op, &err), ToString(f.op));
                    m_bodies[size_t(bi)].lastFeature = f.id;
                    bodyOf[f.id] = m_bodies[size_t(bi)].id;
                };
                switch (f.type) {
                    case FeatureType::Sketch: {
                        Sketch s = f.sketch;
                        for (const Constraint& c : s.Constraints()) {
                            if (c.expression.empty()) continue;
                            double v = 0;
                            std::string e;
                            if (!Eval(c.expression, v, &e)) Fail("dimension " + e);
                            if (!s.SetConstraintValue(c.id, v)) Fail("dimension value " + std::to_string(v) + " is not allowed");
                        }
                        st.solve = s.Solve();
                        if (st.solve.status != SolveStatus::Solved) Fail("sketch: " + st.solve.message);
                        Plane p = f.plane == "XZ" ? Plane::XZ() : f.plane == "YZ" ? Plane::YZ() : f.plane == "custom" ? f.frame : Plane::XY();
                        p.origin += p.Normal() * num(f, "offset");
                        solved[f.id] = s;
                        planes[f.id] = p;
                        break;
                    }
                    case FeatureType::Extrude: case FeatureType::Revolve: {
                        auto sk = solved.find(f.profileSketch);
                        if (sk == solved.end()) Fail("choose a sketch with a closed profile");
                        const ProfileResult pr = sk->second.Profiles();
                        if (!pr.ok()) Fail(pr.error);
                        const Plane& plane = planes[f.profileSketch];
                        const Shape faces = check(Kernel::Faces(pr.profiles, plane, &err), "profile");
                        Shape tool;
                        if (f.type == FeatureType::Extrude) {
                            const double distance = num(f, "distance");
                            if (!(std::fabs(distance) > 1e-9)) Fail("distance must not be zero");
                            tool = check(Kernel::Extrude(faces, plane.Normal() * distance * (f.reverse ? -1.0 : 1.0), f.symmetric, &err), "extrude");
                        }
                        else {
                            glm::dvec3 origin = plane.origin, dir = f.axis == "x" ? plane.u : plane.v;
                            if (f.axis == "line") {
                                const Geometry* line = sk->second.Find(f.axisLine);
                                if (!line || line->type != GeometryType::Line) Fail("the axis line is not in the sketch");
                                origin = plane.ToWorld(sk->second.PointPosition(line->a));
                                dir = plane.ToWorld(sk->second.PointPosition(line->b)) - origin;
                            }
                            const double angle = num(f, "angle");
                            if (!(std::fabs(angle) > 1e-9) || std::fabs(angle) > 360.0) Fail("angle must be in (0, 360] degrees");
                            tool = check(Kernel::Revolve(faces, origin, dir * (f.reverse ? -1.0 : 1.0), angle, &err), "revolve");
                        }
                        combine(tool, f.id);
                        break;
                    }
                    case FeatureType::Box: {
                        const glm::dvec3 size(num(f, "x"), num(f, "y"), num(f, "z"));
                        if (!(size.x > 0 && size.y > 0 && size.z > 0)) Fail("box sizes must be positive");
                        combine(check(Kernel::Box(f.position, size, &err), "box"), f.id);
                        break;
                    }
                    case FeatureType::Cylinder: {
                        const double r = num(f, "radius"), h = num(f, "height");
                        if (!(r > 0 && h > 0)) Fail("radius and height must be positive");
                        combine(check(Kernel::Cylinder(f.position, f.direction, r, h, &err), "cylinder"), f.id);
                        break;
                    }
                    case FeatureType::Sphere: {
                        const double r = num(f, "radius");
                        if (!(r > 0)) Fail("radius must be positive");
                        combine(check(Kernel::Sphere(f.position, r, &err), "sphere"), f.id);
                        break;
                    }
                    case FeatureType::Import: {
                        Shape s;
                        if (!f.brep.empty()) s = check(Kernel::FromBrep(f.brep, &err), "reading the imported geometry");
                        else {
                            const std::string path = resolver ? resolver(f.asset) : std::string();
                            if (path.empty()) Fail("the imported file '" + f.asset + "' is not in the project");
                            s = check(ImportFile(path, unit, &err), "import");
                        }
                        combine(s, f.id);
                        break;
                    }
                    case FeatureType::Fillet: case FeatureType::Chamfer: {
                        const int bi = target();
                        const Shape base = m_bodies[size_t(bi)].shape;
                        m_before[f.id] = base;
                        std::vector<int> edges;
                        if (!ResolveEdges(base, f.edges, edges, &err)) Fail(err);
                        const double r = num(f, f.type == FeatureType::Fillet ? "radius" : "distance");
                        if (!(r > 0)) Fail("size must be positive");
                        m_bodies[size_t(bi)].shape = f.type == FeatureType::Fillet ? check(Kernel::Fillet(base, edges, r, &err), "fillet")
                                                                                 : check(Kernel::Chamfer(base, edges, r, &err), "chamfer");
                        m_bodies[size_t(bi)].lastFeature = f.id;
                        bodyOf[f.id] = m_bodies[size_t(bi)].id;
                        break;
                    }
                    case FeatureType::Boolean: {
                        if (!f.tool) Fail("choose the tool body");
                        const int ti = bodyIndex(f.tool);
                        if (ti < 0) Fail("the tool body does not exist here");
                        const int bi = target();
                        if (bi == ti) Fail("a body cannot be combined with itself");
                        m_before[f.id] = m_bodies[size_t(bi)].shape;
                        m_bodies[size_t(bi)].shape = check(Kernel::Boolean(m_bodies[size_t(bi)].shape, m_bodies[size_t(ti)].shape, f.boolean, &err), BooleanName(f.boolean));
                        m_bodies[size_t(bi)].lastFeature = f.id;
                        bodyOf[f.id] = m_bodies[size_t(bi)].id;
                        m_bodies.erase(m_bodies.begin() + ti);
                        break;
                    }
                    case FeatureType::LinearPattern: case FeatureType::CircularPattern: case FeatureType::Mirror: {
                        const bool mirror = f.type == FeatureType::Mirror;
                        int count = 0;
                        if (!mirror) {
                            const double c = num(f, "count");
                            if (!(c >= 2 && c <= 1000) || std::fabs(c - std::round(c)) > 1e-9) Fail("count must be a whole number from 2 to 1000");
                            count = int(std::round(c));
                        }
                        auto pattern = [&](const Shape& s) {
                            if (f.type == FeatureType::LinearPattern) {
                                if (!(glm::length(f.direction) > 1e-12)) Fail("choose a direction");
                                return check(Kernel::LinearPattern(s, glm::normalize(f.direction) * num(f, "spacing"), count, &err), "pattern");
                            }
                            if (f.type == FeatureType::CircularPattern) return check(Kernel::CircularPattern(s, f.origin, f.direction, count, num(f, "angle"), &err), "pattern");
                            return check(Kernel::Mirror(s, f.origin, f.direction, f.keepOriginal || f.source, &err), "mirror");
                        };
                        if (f.source) {   // repeat a feature's tool on the body that feature built / modified
                            const Feature* src = FindFeature(f.source);
                            auto tool = m_tools.find(f.source);
                            if (!src || tool == m_tools.end() || !bodyOf.count(f.source)) Fail("the source feature has no tool shape (extrude, revolve or a primitive)");
                            const int bi = bodyIndex(bodyOf[f.source]);
                            if (bi < 0) Fail("the source feature's body no longer exists");
                            m_before[f.id] = m_bodies[size_t(bi)].shape;
                            const BooleanOp op = src->op == FeatureOp::Cut ? BooleanOp::Subtract : src->op == FeatureOp::Intersect ? BooleanOp::Intersect : BooleanOp::Union;
                            m_bodies[size_t(bi)].shape = check(Kernel::Boolean(m_bodies[size_t(bi)].shape, pattern(tool->second), op, &err), "pattern");
                            m_bodies[size_t(bi)].lastFeature = f.id;
                            bodyOf[f.id] = m_bodies[size_t(bi)].id;
                        }
                        else {
                            const int bi = target();
                            m_before[f.id] = m_bodies[size_t(bi)].shape;
                            m_bodies[size_t(bi)].shape = pattern(m_bodies[size_t(bi)].shape);
                            m_bodies[size_t(bi)].lastFeature = f.id;
                            bodyOf[f.id] = m_bodies[size_t(bi)].id;
                        }
                        break;
                    }
                }
                st.built = true;
                if (bodyOf.count(f.id)) {
                    const int bi = bodyIndex(bodyOf[f.id]);
                    if (bi >= 0) {
                        m_after[f.id] = m_bodies[size_t(bi)].shape;
                        st.properties = Kernel::Measure(m_bodies[size_t(bi)].shape);
                        if (!st.properties.valid) Fail("the result is not a valid solid");
                    }
                }
            }
            catch (const std::exception& e) {
                st.built = false;
                st.error = e.what();
                failed.insert(f.id);
                ++result.failed;
                if (st.error.empty()) st.error = "failed";
                if (result.firstError.empty()) result.firstError = f.name + ": " + st.error;
            }
            st.milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - ft).count();
        }
        result.built = 0;
        for (const auto& [id, s] : m_state) if (s.built) ++result.built;
        result.ok = result.failed == 0 && result.skipped == 0 && paramError.empty();
        if (!paramError.empty() && result.firstError.empty()) result.firstError = paramError;
        result.milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        return result;
    }

    // ======================================================================== exchange
    Shape Document::ImportFile(const std::string& path, Unit unit, std::string* error) {
        FileFormat format;
        if (!FormatFromPath(path, format)) { if (error) *error = "unsupported file type (STEP, IGES, BREP or STL)"; return {}; }
        const Shape s = Kernel::Import(path, format, error);
        if (!s) return {};
        const double scale = 0.001 / MetresPerUnit(unit);   // millimetres -> document unit
        if (std::fabs(scale - 1.0) < 1e-12) return s;
        return Kernel::Transform(s, glm::dmat4(glm::dvec4(scale, 0, 0, 0), glm::dvec4(0, scale, 0, 0), glm::dvec4(0, 0, scale, 0), glm::dvec4(0, 0, 0, 1)), error);
    }
    bool Document::ExportShape(const Shape& shape, Unit unit, const std::string& path, FileFormat format, std::string* error) {
        if (!shape) { if (error) *error = "nothing to export (the model is empty or failed to build)"; return false; }
        const double scale = MetresPerUnit(unit) / 0.001;   // document unit -> millimetres
        Shape s = shape;
        if (std::fabs(scale - 1.0) > 1e-12) {
            s = Kernel::Transform(shape, glm::dmat4(glm::dvec4(scale, 0, 0, 0), glm::dvec4(0, scale, 0, 0), glm::dvec4(0, 0, scale, 0), glm::dvec4(0, 0, 0, 1)), error);
            if (!s) return false;
        }
        const Properties p = Kernel::Measure(s);
        const double diag = glm::length(p.boundsMax - p.boundsMin);
        return Kernel::Export(s, path, format, error, std::max(diag * 0.0005, 0.01));
    }
    bool Document::ExportResult(const std::string& path, FileFormat format, std::string* error) const {
        return ExportShape(Result(), unit, path, format, error);
    }

    // ======================================================================== JSON
    std::string Document::ToJson(bool pretty) const {
        json params = json::array();
        for (const Parameter& p : m_parameters) params.push_back({ { "name", p.name }, { "expression", p.expression } });
        json features = json::array();
        for (const Feature& f : m_features) {
            json j = { { "id", f.id }, { "type", ToString(f.type) }, { "name", f.name } };
            if (f.suppressed) j["suppressed"] = true;
            if (!f.values.empty()) j["values"] = f.values;
            switch (f.type) {
                case FeatureType::Sketch:
                    j["plane"] = f.plane;
                    if (f.plane == "custom") j["frame"] = { { "origin", V3(f.frame.origin) }, { "u", V3(f.frame.u) }, { "v", V3(f.frame.v) } };
                    j["sketch"] = json::parse(f.sketch.ToJson());
                    break;
                case FeatureType::Extrude: case FeatureType::Revolve:
                    j["sketch"] = f.profileSketch;
                    if (f.symmetric) j["symmetric"] = true;
                    if (f.reverse) j["reverse"] = true;
                    if (f.type == FeatureType::Revolve) { j["axis"] = f.axis; if (f.axis == "line") j["axisLine"] = f.axisLine; }
                    break;
                case FeatureType::Box: case FeatureType::Sphere: j["position"] = V3(f.position); break;
                case FeatureType::Cylinder: j["position"] = V3(f.position); j["direction"] = V3(f.direction); break;
                case FeatureType::Fillet: case FeatureType::Chamfer: {
                    json e = json::array();
                    for (const EdgeRef& r : f.edges) e.push_back({ { "midpoint", V3(r.midpoint) }, { "tangent", V3(r.tangent) } });
                    j["edges"] = e;
                    break;
                }
                case FeatureType::Boolean: j["boolean"] = BooleanName(f.boolean); j["tool"] = f.tool; break;
                case FeatureType::LinearPattern: j["direction"] = V3(f.direction); if (f.source) j["source"] = f.source; break;
                case FeatureType::CircularPattern: j["origin"] = V3(f.origin); j["direction"] = V3(f.direction); if (f.source) j["source"] = f.source; break;
                case FeatureType::Mirror: j["origin"] = V3(f.origin); j["direction"] = V3(f.direction); j["keepOriginal"] = f.keepOriginal; break;
                case FeatureType::Import: j["asset"] = f.asset; if (!f.brep.empty()) j["brep"] = f.brep; break;
            }
            if (MakesBody(f)) j["op"] = ToString(f.op);
            if (f.body) j["body"] = f.body;
            features.push_back(j);
        }
        json doc = { { "format", "phisiks-cad" }, { "version", FORMAT_VERSION }, { "unit", UnitSymbol(unit) },
                     { "parameters", params }, { "features", features }, { "nextId", m_nextId } };
        if (rollback != size_t(-1)) doc["rollback"] = rollback;
        return pretty ? doc.dump(2) : doc.dump();
    }

    bool Document::FromJson(const std::string& text, Document& out, std::string* error) {
        try {
            const json j = json::parse(text);
            if (j.value("format", std::string()) != "phisiks-cad") Fail("not a PHISIKS CAD document");
            const int version = j.value("version", 0);
            if (version < 1 || version > FORMAT_VERSION) Fail("document version " + std::to_string(version) + " is not supported by this PHISIKS (" + std::to_string(FORMAT_VERSION) + ")");
            Document d;
            if (!ParseUnit(j.value("unit", std::string("mm")).c_str(), d.unit)) Fail("unknown unit");
            for (const json& p : j.value("parameters", json::array())) {
                Parameter x;
                x.name = p.at("name").get<std::string>();
                x.expression = p.at("expression").get<std::string>();
                if (!IsValidParameterName(x.name)) Fail("invalid parameter name '" + x.name + "'");
                d.m_parameters.push_back(x);
            }
            Id maxId = 0;
            for (const json& fj : j.value("features", json::array())) {
                Feature f;
                f.id = fj.at("id").get<Id>();
                if (!ParseFeatureType(fj.at("type").get<std::string>(), f.type)) Fail("unknown feature type '" + fj.at("type").get<std::string>() + "'");
                if (!f.id || d.FindFeature(f.id)) Fail("duplicate or zero feature id");
                f.name = fj.value("name", std::string());
                f.suppressed = fj.value("suppressed", false);
                if (fj.contains("values")) for (auto it = fj.at("values").begin(); it != fj.at("values").end(); ++it) f.values[it.key()] = it.value().get<std::string>();
                for (const auto& [k, v] : Defaults(f.type)) if (!f.values.count(k)) f.values[k] = v;
                f.plane = fj.value("plane", std::string("XY"));
                if (fj.contains("frame")) {
                    const json& fr = fj.at("frame");
                    f.frame.origin = V3(fr.at("origin"), glm::dvec3(0.0));
                    f.frame.u = V3(fr.at("u"), glm::dvec3(1, 0, 0));
                    f.frame.v = V3(fr.at("v"), glm::dvec3(0, 1, 0));
                }
                if (f.type == FeatureType::Sketch) {
                    std::string e;
                    if (fj.contains("sketch") && !Sketch::FromJson(fj.at("sketch").dump(), f.sketch, &e)) Fail(f.name + ": " + e);
                    if (f.plane != "XY" && f.plane != "XZ" && f.plane != "YZ" && f.plane != "custom") Fail("unknown sketch plane '" + f.plane + "'");
                }
                else f.profileSketch = fj.value("sketch", Id(0));
                f.symmetric = fj.value("symmetric", false);
                f.reverse = fj.value("reverse", false);
                f.axis = fj.value("axis", std::string("y"));
                f.axisLine = fj.value("axisLine", Id(0));
                if (fj.contains("op") && !ParseOp(fj.at("op").get<std::string>(), f.op)) Fail("unknown operation");
                f.body = fj.value("body", Id(0));
                if (fj.contains("position")) f.position = V3(fj.at("position"), f.position);
                if (fj.contains("direction")) f.direction = V3(fj.at("direction"), f.direction);
                if (fj.contains("origin")) f.origin = V3(fj.at("origin"), f.origin);
                for (const json& e : fj.value("edges", json::array())) f.edges.push_back({ V3(e.at("midpoint"), glm::dvec3(0.0)), V3(e.at("tangent"), glm::dvec3(0.0)) });
                if (fj.contains("boolean") && !ParseBoolean(fj.at("boolean").get<std::string>(), f.boolean)) Fail("unknown boolean");
                f.tool = fj.value("tool", Id(0));
                f.source = fj.value("source", Id(0));
                f.keepOriginal = fj.value("keepOriginal", true);
                f.asset = fj.value("asset", std::string());
                f.brep = fj.value("brep", std::string());
                maxId = std::max(maxId, f.id);
                d.m_features.push_back(std::move(f));
            }
            for (size_t k = 0; k < d.m_features.size(); ++k)
                for (Id dep : d.Dependencies(d.m_features[k])) {
                    const size_t di = d.IndexOf(dep);
                    if (di == size_t(-1) || di >= k) Fail(d.m_features[k].name + " refers to a feature that is missing or comes after it");
                }
            d.m_nextId = std::max<Id>(j.value("nextId", Id(1)), maxId + 1);
            if (j.contains("rollback")) d.rollback = j.at("rollback").get<size_t>();
            std::string pe;
            d.EvaluateParameters(&pe);   // errors are kept per parameter and reported by Regenerate
            out = std::move(d);
            return true;
        }
        catch (const std::exception& e) {
            if (error) *error = std::string("CAD document: ") + e.what();
            return false;
        }
    }

}
