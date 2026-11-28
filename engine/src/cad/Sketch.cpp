// Sketch geometry, constraints and solver (see Phisiks/Cad/Sketch.h).
#include "Phisiks/Cad/Sketch.h"

#include "CadJson.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>

namespace Phisiks::Cad {
    namespace {
        constexpr double PI = 3.14159265358979323846;
        double Cross(glm::dvec2 a, glm::dvec2 b) { return a.x * b.y - a.y * b.x; }
        double Dot(glm::dvec2 a, glm::dvec2 b) { return a.x * b.x + a.y * b.y; }
        double Len(glm::dvec2 a) { return std::sqrt(Dot(a, a)); }
        double WrapAngle(double a) { while (a > PI) a -= 2 * PI; while (a < -PI) a += 2 * PI; return a; }

        struct TypeName { ConstraintType type; const char* name; };
        const TypeName CONSTRAINT_NAMES[] = {
            { ConstraintType::Coincident, "coincident" }, { ConstraintType::Horizontal, "horizontal" }, { ConstraintType::Vertical, "vertical" },
            { ConstraintType::Parallel, "parallel" }, { ConstraintType::Perpendicular, "perpendicular" }, { ConstraintType::Tangent, "tangent" },
            { ConstraintType::Equal, "equal" }, { ConstraintType::Concentric, "concentric" }, { ConstraintType::PointOnLine, "pointOnLine" },
            { ConstraintType::PointOnCircle, "pointOnCircle" }, { ConstraintType::Midpoint, "midpoint" }, { ConstraintType::Symmetric, "symmetric" },
            { ConstraintType::Fix, "fix" }, { ConstraintType::Distance, "distance" }, { ConstraintType::HorizontalDistance, "horizontalDistance" },
            { ConstraintType::VerticalDistance, "verticalDistance" }, { ConstraintType::Length, "length" },
            { ConstraintType::PointLineDistance, "pointLineDistance" }, { ConstraintType::Radius, "radius" }, { ConstraintType::Diameter, "diameter" },
            { ConstraintType::Angle, "angle" },
        };
    }

    const char* ToString(GeometryType type) {
        switch (type) {
            case GeometryType::Point: return "point";
            case GeometryType::Line: return "line";
            case GeometryType::Circle: return "circle";
            case GeometryType::Arc: return "arc";
        }
        return "?";
    }
    const char* ToString(ConstraintType type) {
        for (const TypeName& t : CONSTRAINT_NAMES) if (t.type == type) return t.name;
        return "?";
    }
    bool ParseConstraintType(const std::string& text, ConstraintType& out) {
        for (const TypeName& t : CONSTRAINT_NAMES) if (text == t.name) { out = t.type; return true; }
        return false;
    }
    bool IsDimension(ConstraintType type) {
        switch (type) {
            case ConstraintType::Distance: case ConstraintType::HorizontalDistance: case ConstraintType::VerticalDistance:
            case ConstraintType::Length: case ConstraintType::PointLineDistance: case ConstraintType::Radius:
            case ConstraintType::Diameter: case ConstraintType::Angle: return true;
            default: return false;
        }
    }

    // ======================================================================== geometry
    const Geometry* Sketch::Find(Id id) const {
        for (const Geometry& g : m_geometry) if (g.id == id) return &g;
        return nullptr;
    }
    Geometry* Sketch::Find(Id id) {
        for (Geometry& g : m_geometry) if (g.id == id) return &g;
        return nullptr;
    }
    bool Sketch::IsPoint(Id id) const { const Geometry* g = Find(id); return g && g->type == GeometryType::Point; }
    bool Sketch::IsLine(Id id) const { const Geometry* g = Find(id); return g && g->type == GeometryType::Line; }
    bool Sketch::IsRound(Id id) const { const Geometry* g = Find(id); return g && (g->type == GeometryType::Circle || g->type == GeometryType::Arc); }

    Id Sketch::AddPoint(double x, double y, bool construction) {
        if (!std::isfinite(x) || !std::isfinite(y)) return 0;
        Geometry g;
        g.id = NewId(); g.type = GeometryType::Point; g.x = x; g.y = y; g.construction = construction;
        m_geometry.push_back(g);
        return g.id;
    }
    Id Sketch::AddLine(Id p1, Id p2, bool construction) {
        if (p1 == p2 || !IsPoint(p1) || !IsPoint(p2)) return 0;
        Geometry g;
        g.id = NewId(); g.type = GeometryType::Line; g.a = p1; g.b = p2; g.construction = construction;
        m_geometry.push_back(g);
        return g.id;
    }
    Id Sketch::AddLine(double x1, double y1, double x2, double y2, bool construction) {
        const Id a = AddPoint(x1, y1), b = AddPoint(x2, y2);
        if (!a || !b) return 0;
        return AddLine(a, b, construction);
    }
    Id Sketch::AddCircle(Id centre, double radius, bool construction) {
        if (!IsPoint(centre) || !(radius > 0.0) || !std::isfinite(radius)) return 0;
        Geometry g;
        g.id = NewId(); g.type = GeometryType::Circle; g.a = centre; g.radius = radius; g.construction = construction;
        m_geometry.push_back(g);
        return g.id;
    }
    Id Sketch::AddCircleAt(double cx, double cy, double radius, bool construction) {
        if (!(radius > 0.0)) return 0;
        return AddCircle(AddPoint(cx, cy), radius, construction);
    }
    Id Sketch::AddArc(Id centre, Id start, Id end, bool construction) {
        if (!IsPoint(centre) || !IsPoint(start) || !IsPoint(end) || centre == start || centre == end || start == end) return 0;
        Geometry g;
        g.id = NewId(); g.type = GeometryType::Arc; g.a = centre; g.b = start; g.c = end; g.construction = construction;
        m_geometry.push_back(g);
        return g.id;
    }
    Id Sketch::AddArc(double cx, double cy, double radius, double startDegrees, double endDegrees, bool construction) {
        if (!(radius > 0.0)) return 0;
        const double s = startDegrees * PI / 180.0, e = endDegrees * PI / 180.0;
        const Id c = AddPoint(cx, cy);
        const Id p1 = AddPoint(cx + radius * std::cos(s), cy + radius * std::sin(s));
        const Id p2 = AddPoint(cx + radius * std::cos(e), cy + radius * std::sin(e));
        return AddArc(c, p1, p2, construction);
    }
    std::vector<Id> Sketch::AddRectangle(double x1, double y1, double x2, double y2) {
        if (x1 == x2 || y1 == y2) return {};
        const Id a = AddPoint(x1, y1), b = AddPoint(x2, y1), c = AddPoint(x2, y2), d = AddPoint(x1, y2);
        std::vector<Id> lines = { AddLine(a, b), AddLine(b, c), AddLine(c, d), AddLine(d, a) };
        AddConstraint(ConstraintType::Horizontal, lines[0]);
        AddConstraint(ConstraintType::Vertical, lines[1]);
        AddConstraint(ConstraintType::Horizontal, lines[2]);
        AddConstraint(ConstraintType::Vertical, lines[3]);
        return lines;
    }
    bool Sketch::Remove(Id id) {
        const Geometry* g = Find(id);
        if (!g) return false;
        std::vector<Id> remove = { id };
        if (g->type == GeometryType::Point)   // geometry built on the point goes too
            for (const Geometry& o : m_geometry) if (o.type != GeometryType::Point && (o.a == id || o.b == id || o.c == id)) remove.push_back(o.id);
        if (g->type == GeometryType::Circle) remove.push_back(g->a);   // a circle's centre is its own
        for (Id r : remove) {
            m_constraints.erase(std::remove_if(m_constraints.begin(), m_constraints.end(),
                [&](const Constraint& c) { return c.a == r || c.b == r || c.c == r; }), m_constraints.end());
        }
        // A circle's centre may be shared (concentric geometry): keep it if anything else uses it.
        auto used = [&](Id p) {
            for (const Geometry& o : m_geometry)
                if (std::find(remove.begin(), remove.end(), o.id) == remove.end() && (o.a == p || o.b == p || o.c == p)) return true;
            return false;
        };
        remove.erase(std::remove_if(remove.begin() + 1, remove.end(), [&](Id r) { const Geometry* x = Find(r); return x && x->type == GeometryType::Point && used(r); }), remove.end());
        m_geometry.erase(std::remove_if(m_geometry.begin(), m_geometry.end(),
            [&](const Geometry& x) { return std::find(remove.begin(), remove.end(), x.id) != remove.end(); }), m_geometry.end());
        return true;
    }
    bool Sketch::SetPoint(Id point, double x, double y) {
        Geometry* g = Find(point);
        if (!g || g->type != GeometryType::Point || !std::isfinite(x) || !std::isfinite(y)) return false;
        g->x = x; g->y = y;
        return true;
    }
    bool Sketch::SetRadius(Id circle, double radius) {
        Geometry* g = Find(circle);
        if (!g || !(radius > 0.0) || !std::isfinite(radius)) return false;
        if (g->type == GeometryType::Circle) { g->radius = radius; return true; }
        if (g->type == GeometryType::Arc) {
            const glm::dvec2 c = PointPosition(g->a);
            for (Id p : { g->b, g->c }) {
                glm::dvec2 d = PointPosition(p) - c;
                const double l = Len(d);
                if (l > 0) SetPoint(p, c.x + d.x / l * radius, c.y + d.y / l * radius);
            }
            return true;
        }
        return false;
    }
    bool Sketch::SetConstruction(Id id, bool construction) {
        Geometry* g = Find(id);
        if (!g) return false;
        g->construction = construction;
        return true;
    }
    glm::dvec2 Sketch::PointPosition(Id point) const {
        const Geometry* g = Find(point);
        return g ? glm::dvec2(g->x, g->y) : glm::dvec2(0.0);
    }
    double Sketch::RadiusOf(Id id) const {
        const Geometry* g = Find(id);
        if (!g) return 0.0;
        if (g->type == GeometryType::Circle) return g->radius;
        if (g->type == GeometryType::Arc) return Len(PointPosition(g->b) - PointPosition(g->a));
        return 0.0;
    }

    // ======================================================================== constraints
    Id Sketch::AddConstraint(ConstraintType type, Id a, Id b, Id c, double value) {
        bool ok = false;
        switch (type) {
            case ConstraintType::Coincident: ok = IsPoint(a) && IsPoint(b) && a != b; break;
            case ConstraintType::Horizontal: case ConstraintType::Vertical: ok = IsLine(a) || (IsPoint(a) && IsPoint(b) && a != b); break;
            case ConstraintType::Parallel: case ConstraintType::Perpendicular: ok = IsLine(a) && IsLine(b) && a != b; break;
            case ConstraintType::Tangent: ok = (IsLine(a) && IsRound(b)) || (IsRound(a) && IsRound(b) && a != b); break;
            case ConstraintType::Equal: ok = a != b && ((IsLine(a) && IsLine(b)) || (IsRound(a) && IsRound(b))); break;
            case ConstraintType::Concentric: ok = IsRound(a) && IsRound(b) && a != b; break;
            case ConstraintType::PointOnLine: case ConstraintType::Midpoint: ok = IsPoint(a) && IsLine(b); break;
            case ConstraintType::PointOnCircle: ok = IsPoint(a) && IsRound(b); break;
            case ConstraintType::Symmetric: ok = IsPoint(a) && IsPoint(b) && a != b && IsLine(c); break;
            case ConstraintType::Fix: ok = IsPoint(a); break;
            case ConstraintType::Distance: case ConstraintType::HorizontalDistance: case ConstraintType::VerticalDistance:
                ok = IsPoint(a) && IsPoint(b) && a != b; break;
            case ConstraintType::Length: ok = IsLine(a); break;
            case ConstraintType::PointLineDistance: ok = IsPoint(a) && IsLine(b); break;
            case ConstraintType::Radius: case ConstraintType::Diameter: ok = IsRound(a); break;
            case ConstraintType::Angle: ok = IsLine(a) && IsLine(b) && a != b; break;
        }
        if (!ok || !std::isfinite(value)) return 0;
        if ((type == ConstraintType::Distance || type == ConstraintType::Length || type == ConstraintType::Radius || type == ConstraintType::Diameter) && value < 0.0) return 0;
        Constraint con;
        con.id = NewId(); con.type = type; con.a = a; con.b = b; con.c = c; con.value = value;
        if (type == ConstraintType::Tangent) {
            // Keep the side the geometry is on now (line: which side of the line the centre lies; circles: outside / inside).
            const Geometry* ga = Find(a);
            if (ga->type == GeometryType::Line) {
                const glm::dvec2 p1 = PointPosition(ga->a), p2 = PointPosition(ga->b), ctr = PointPosition(Find(b)->a);
                con.side = Cross(p2 - p1, ctr - p1) >= 0.0 ? 1 : -1;
            }
            else {
                const double d = Len(PointPosition(Find(a)->a) - PointPosition(Find(b)->a));
                con.side = d >= std::max(RadiusOf(a), RadiusOf(b)) ? 1 : -1;   // +1 external, -1 internal
            }
        }
        if (type == ConstraintType::Fix) Find(a)->fixed = true;
        m_constraints.push_back(con);
        return con.id;
    }
    Id Sketch::AddDimension(ConstraintType type, Id a, Id b) {
        if (!IsDimension(type)) return 0;
        double v = Measure(type, a, b);
        if (type == ConstraintType::PointLineDistance) v = std::fabs(v);
        return AddConstraint(type, a, b, 0, v);
    }
    bool Sketch::RemoveConstraint(Id id) {
        for (size_t i = 0; i < m_constraints.size(); ++i) {
            if (m_constraints[i].id != id) continue;
            if (m_constraints[i].type == ConstraintType::Fix) {
                const Id p = m_constraints[i].a;
                bool other = false;
                for (const Constraint& c : m_constraints) if (c.id != id && c.type == ConstraintType::Fix && c.a == p) other = true;
                if (!other) if (Geometry* g = Find(p)) g->fixed = false;
            }
            m_constraints.erase(m_constraints.begin() + long(i));
            return true;
        }
        return false;
    }
    bool Sketch::SetConstraintValue(Id id, double value) {
        for (Constraint& c : m_constraints) {
            if (c.id != id || !IsDimension(c.type) || !std::isfinite(value)) continue;
            if (c.type != ConstraintType::Angle && c.type != ConstraintType::HorizontalDistance && c.type != ConstraintType::VerticalDistance && value < 0.0) return false;
            c.value = value;
            return true;
        }
        return false;
    }
    bool Sketch::SetConstraintExpression(Id id, const std::string& expression) {
        for (Constraint& c : m_constraints) if (c.id == id && IsDimension(c.type)) { c.expression = expression; return true; }
        return false;
    }
    const Constraint* Sketch::FindConstraint(Id id) const {
        for (const Constraint& c : m_constraints) if (c.id == id) return &c;
        return nullptr;
    }

    double Sketch::Measure(ConstraintType type, Id a, Id b) const {
        auto line = [&](Id l, glm::dvec2& p1, glm::dvec2& p2) { const Geometry* g = Find(l); p1 = PointPosition(g->a); p2 = PointPosition(g->b); };
        switch (type) {
            case ConstraintType::Distance: if (IsPoint(a) && IsPoint(b)) return Len(PointPosition(b) - PointPosition(a)); break;
            case ConstraintType::HorizontalDistance: if (IsPoint(a) && IsPoint(b)) return PointPosition(b).x - PointPosition(a).x; break;
            case ConstraintType::VerticalDistance: if (IsPoint(a) && IsPoint(b)) return PointPosition(b).y - PointPosition(a).y; break;
            case ConstraintType::Length: if (IsLine(a)) { glm::dvec2 p1, p2; line(a, p1, p2); return Len(p2 - p1); } break;
            case ConstraintType::PointLineDistance:
                if (IsPoint(a) && IsLine(b)) { glm::dvec2 p1, p2; line(b, p1, p2); const double l = Len(p2 - p1); return l > 0 ? Cross(p2 - p1, PointPosition(a) - p1) / l : 0.0; }
                break;
            case ConstraintType::Radius: if (IsRound(a)) return RadiusOf(a); break;
            case ConstraintType::Diameter: if (IsRound(a)) return 2.0 * RadiusOf(a); break;
            case ConstraintType::Angle:
                if (IsLine(a) && IsLine(b)) {
                    glm::dvec2 a1, a2, b1, b2; line(a, a1, a2); line(b, b1, b2);
                    return std::atan2(Cross(a2 - a1, b2 - b1), Dot(a2 - a1, b2 - b1)) * 180.0 / PI;
                }
                break;
            default: break;
        }
        return 0.0;
    }

    // ======================================================================== solver
    struct SketchSolver {
        Sketch& s;
        std::vector<double*> vars;                    // free parameters
        std::map<Id, size_t> index;                   // geometry id -> position in m_geometry
        struct Eq { Id constraint; std::function<double()> f; };
        std::vector<Eq> eqs;

        explicit SketchSolver(Sketch& sk) : s(sk) {
            for (size_t i = 0; i < s.m_geometry.size(); ++i) index[s.m_geometry[i].id] = i;
            for (Geometry& g : s.m_geometry) {
                if (g.type == GeometryType::Point && !g.fixed) { vars.push_back(&g.x); vars.push_back(&g.y); }
                if (g.type == GeometryType::Circle) vars.push_back(&g.radius);
            }
            Build();
        }
        Geometry& G(Id id) { return s.m_geometry[index.at(id)]; }
        glm::dvec2 P(Id id) { const Geometry& g = G(id); return { g.x, g.y }; }
        double R(Id id) { const Geometry& g = G(id); return g.type == GeometryType::Circle ? g.radius : Len(P(g.b) - P(g.a)); }
        glm::dvec2 C(Id round) { return P(G(round).a); }

        void Add(Id c, std::function<double()> f) { eqs.push_back({ c, std::move(f) }); }
        void Build() {
            // implicit: arcs keep both ends on one circle
            for (const Geometry& g : s.m_geometry)
                if (g.type == GeometryType::Arc) { const Id a = g.a, b = g.b, c = g.c; Add(0, [=] { return Len(P(b) - P(a)) - Len(P(c) - P(a)); }); }
            for (const Constraint& con : s.m_constraints) {
                const Id a = con.a, b = con.b, c = con.c, id = con.id;
                const double v = con.value;
                const int side = con.side;
                auto L1 = [this](Id l) { return P(G(l).a); };
                auto L2 = [this](Id l) { return P(G(l).b); };
                switch (con.type) {
                    case ConstraintType::Coincident:
                        Add(id, [=] { return P(a).x - P(b).x; }); Add(id, [=] { return P(a).y - P(b).y; }); break;
                    case ConstraintType::Horizontal:
                        if (s.IsLine(a)) Add(id, [=] { return L1(a).y - L2(a).y; }); else Add(id, [=] { return P(a).y - P(b).y; });
                        break;
                    case ConstraintType::Vertical:
                        if (s.IsLine(a)) Add(id, [=] { return L1(a).x - L2(a).x; }); else Add(id, [=] { return P(a).x - P(b).x; });
                        break;
                    case ConstraintType::Parallel:
                        Add(id, [=] { glm::dvec2 d1 = L2(a) - L1(a), d2 = L2(b) - L1(b); const double n = Len(d1) * Len(d2); return n > 0 ? Cross(d1, d2) / n : 0.0; }); break;
                    case ConstraintType::Perpendicular:
                        Add(id, [=] { glm::dvec2 d1 = L2(a) - L1(a), d2 = L2(b) - L1(b); const double n = Len(d1) * Len(d2); return n > 0 ? Dot(d1, d2) / n : 0.0; }); break;
                    case ConstraintType::Tangent:
                        if (s.IsLine(a)) Add(id, [=] { glm::dvec2 p1 = L1(a), d = L2(a) - p1; const double l = Len(d); return l > 0 ? side * Cross(d, C(b) - p1) / l - R(b) : 0.0; });
                        else if (side > 0) Add(id, [=] { return Len(C(a) - C(b)) - (R(a) + R(b)); });
                        else Add(id, [=] { return Len(C(a) - C(b)) - std::fabs(R(a) - R(b)); });
                        break;
                    case ConstraintType::Equal:
                        if (s.IsLine(a)) Add(id, [=] { return Len(L2(a) - L1(a)) - Len(L2(b) - L1(b)); }); else Add(id, [=] { return R(a) - R(b); });
                        break;
                    case ConstraintType::Concentric:
                        Add(id, [=] { return C(a).x - C(b).x; }); Add(id, [=] { return C(a).y - C(b).y; }); break;
                    case ConstraintType::PointOnLine:
                        Add(id, [=] { glm::dvec2 p1 = L1(b), d = L2(b) - p1; const double l = Len(d); return l > 0 ? Cross(d, P(a) - p1) / l : 0.0; }); break;
                    case ConstraintType::PointOnCircle:
                        Add(id, [=] { return Len(P(a) - C(b)) - R(b); }); break;
                    case ConstraintType::Midpoint:
                        Add(id, [=] { return P(a).x - 0.5 * (L1(b).x + L2(b).x); }); Add(id, [=] { return P(a).y - 0.5 * (L1(b).y + L2(b).y); }); break;
                    case ConstraintType::Symmetric:
                        // the midpoint of a-b lies on line c, and a-b is perpendicular to c
                        Add(id, [=] { glm::dvec2 p1 = L1(c), d = L2(c) - p1; const double l = Len(d); return l > 0 ? Cross(d, 0.5 * (P(a) + P(b)) - p1) / l : 0.0; });
                        Add(id, [=] { glm::dvec2 d = L2(c) - L1(c); const double l = Len(d); return l > 0 ? Dot(d, P(b) - P(a)) / l : 0.0; });
                        break;
                    case ConstraintType::Fix: break;   // the point's parameters are not free
                    case ConstraintType::Distance: Add(id, [=] { return Len(P(b) - P(a)) - v; }); break;
                    case ConstraintType::HorizontalDistance: Add(id, [=] { return P(b).x - P(a).x - v; }); break;
                    case ConstraintType::VerticalDistance: Add(id, [=] { return P(b).y - P(a).y - v; }); break;
                    case ConstraintType::Length: Add(id, [=] { return Len(L2(a) - L1(a)) - v; }); break;
                    case ConstraintType::PointLineDistance:
                        Add(id, [=] { glm::dvec2 p1 = L1(b), d = L2(b) - p1; const double l = Len(d); return l > 0 ? std::fabs(Cross(d, P(a) - p1) / l) - v : 0.0; }); break;
                    case ConstraintType::Radius: Add(id, [=] { return R(a) - v; }); break;
                    case ConstraintType::Diameter: Add(id, [=] { return 2.0 * R(a) - v; }); break;
                    case ConstraintType::Angle:
                        // scaled to lengths so it weighs like the other equations
                        Add(id, [=] {
                            glm::dvec2 d1 = L2(a) - L1(a), d2 = L2(b) - L1(b);
                            const double scale = 0.5 * (Len(d1) + Len(d2));
                            return WrapAngle(std::atan2(Cross(d1, d2), Dot(d1, d2)) - v * PI / 180.0) * scale;
                        });
                        break;
                }
            }
        }

        void Residuals(std::vector<double>& r) { r.resize(eqs.size()); for (size_t i = 0; i < eqs.size(); ++i) r[i] = eqs[i].f(); }
        void Jacobian(std::vector<double>& J) {   // row-major m x n, central differences
            const size_t m = eqs.size(), n = vars.size();
            J.assign(m * n, 0.0);
            std::vector<double> rp(m), rm(m);
            for (size_t j = 0; j < n; ++j) {
                double& x = *vars[j];
                const double x0 = x, h = 1e-7 * std::max(1.0, std::fabs(x0));
                x = x0 + h; Residuals(rp);
                x = x0 - h; Residuals(rm);
                x = x0;
                for (size_t i = 0; i < m; ++i) J[i * n + j] = (rp[i] - rm[i]) / (2.0 * h);
            }
        }
        static double MaxAbs(const std::vector<double>& v) { double m = 0; for (double x : v) m = std::max(m, std::fabs(x)); return m; }
        static bool SolveDense(std::vector<double>& A, std::vector<double>& b, size_t n) {   // Gaussian elimination, partial pivoting
            for (size_t k = 0; k < n; ++k) {
                size_t piv = k;
                for (size_t i = k + 1; i < n; ++i) if (std::fabs(A[i * n + k]) > std::fabs(A[piv * n + k])) piv = i;
                if (std::fabs(A[piv * n + k]) < 1e-300) return false;
                if (piv != k) { for (size_t j = 0; j < n; ++j) std::swap(A[k * n + j], A[piv * n + j]); std::swap(b[k], b[piv]); }
                for (size_t i = k + 1; i < n; ++i) {
                    const double f = A[i * n + k] / A[k * n + k];
                    if (f == 0.0) continue;
                    for (size_t j = k; j < n; ++j) A[i * n + j] -= f * A[k * n + j];
                    b[i] -= f * b[k];
                }
            }
            for (size_t k = n; k-- > 0;) {
                double sum = b[k];
                for (size_t j = k + 1; j < n; ++j) sum -= A[k * n + j] * b[j];
                b[k] = sum / A[k * n + k];
            }
            return true;
        }

        SolveResult Run(int maxIterations, double tolerance) {
            SolveResult out;
            const size_t m = eqs.size(), n = vars.size();
            out.freeParameters = int(n);
            out.equations = int(m);
            std::vector<double> r, J, x0(n), A, g;
            Residuals(r);
            double err = MaxAbs(r);
            if (m > 0 && n > 0) {
                // Minimum-norm damped step: dx = -J^T (J J^T + lambda I)^-1 r  (m x m system), so
                // under-constrained geometry moves as little as possible.
                double lambda = 1e-6;
                for (int it = 0; it < maxIterations && err > tolerance; ++it) {
                    out.iterations = it + 1;
                    Jacobian(J);
                    A.assign(m * m, 0.0);
                    for (size_t i = 0; i < m; ++i)
                        for (size_t k = i; k < m; ++k) {
                            double sum = 0;
                            for (size_t j = 0; j < n; ++j) sum += J[i * n + j] * J[k * n + j];
                            A[i * m + k] = A[k * m + i] = sum;
                        }
                    bool improved = false;
                    for (int tries = 0; tries < 12 && !improved; ++tries) {
                        std::vector<double> M = A, y = r;
                        for (size_t i = 0; i < m; ++i) M[i * m + i] += lambda * (1.0 + A[i * m + i]);
                        if (!SolveDense(M, y, m)) { lambda *= 10; continue; }
                        for (size_t j = 0; j < n; ++j) x0[j] = *vars[j];
                        for (size_t j = 0; j < n; ++j) {
                            double dx = 0;
                            for (size_t i = 0; i < m; ++i) dx += J[i * n + j] * y[i];
                            *vars[j] = x0[j] - dx;
                        }
                        std::vector<double> r2;
                        Residuals(r2);
                        const double e2 = MaxAbs(r2);
                        bool finite = true;
                        for (size_t j = 0; j < n; ++j) finite = finite && std::isfinite(*vars[j]);
                        if (finite && e2 < err) { improved = true; r = r2; err = e2; lambda = std::max(lambda * 0.1, 1e-12); }
                        else { for (size_t j = 0; j < n; ++j) *vars[j] = x0[j]; lambda *= 10; }
                    }
                    if (!improved) break;
                }
            }
            out.residual = err;
            // rank analysis at the result: which equations add no rank?
            if (m > 0) {
                if (n > 0) Jacobian(J);
                std::vector<double> rows;     // orthonormalised accepted rows (Gram-Schmidt)
                size_t rank = 0;
                std::vector<Id> redundant;
                for (size_t i = 0; i < m; ++i) {
                    std::vector<double> v(J.begin() + long(i * n), J.begin() + long(i * n + n));
                    const double norm0 = std::sqrt(std::inner_product(v.begin(), v.end(), v.begin(), 0.0));
                    for (size_t k = 0; k < rank; ++k) {
                        const double d = std::inner_product(v.begin(), v.end(), rows.begin() + long(k * n), 0.0);
                        for (size_t j = 0; j < n; ++j) v[j] -= d * rows[k * n + j];
                    }
                    const double norm = std::sqrt(std::inner_product(v.begin(), v.end(), v.begin(), 0.0));
                    if (norm > 1e-7 * std::max(1.0, norm0) && norm > 1e-10) {
                        for (double& x : v) x /= norm;
                        rows.insert(rows.end(), v.begin(), v.end());
                        ++rank;
                    }
                    else if (eqs[i].constraint && std::find(redundant.begin(), redundant.end(), eqs[i].constraint) == redundant.end())
                        redundant.push_back(eqs[i].constraint);
                }
                out.degreesOfFreedom = int(n) - int(rank);
                out.redundant = redundant;
            }
            else out.degreesOfFreedom = int(n);
            const double scale = std::max(1.0, Extent());
            if (err <= std::max(tolerance, 1e-9 * scale) * 10.0) {
                out.status = SolveStatus::Solved;
                out.message = out.degreesOfFreedom == 0 ? "fully constrained" : std::to_string(out.degreesOfFreedom) + " degrees of freedom left";
                if (!out.redundant.empty()) out.message += ", " + std::to_string(out.redundant.size()) + " redundant constraint(s)";
            }
            else {
                out.status = SolveStatus::Failed;
                out.message = "constraints cannot all be satisfied (residual " + std::to_string(err) + ")";
                if (!out.redundant.empty()) out.message += "; check the conflicting constraints";
            }
            return out;
        }
        double Extent() {
            double e = 0;
            for (const Geometry& g : s.m_geometry) if (g.type == GeometryType::Point) e = std::max({ e, std::fabs(g.x), std::fabs(g.y) });
            return e;
        }
    };

    SolveResult Sketch::Solve(int maxIterations, double tolerance) {
        // Validate references first (a corrupt sketch must not crash the solver).
        for (const Constraint& c : m_constraints) {
            for (Id r : { c.a, c.b, c.c }) if (r && !Find(r)) { m_last = {}; m_last.status = SolveStatus::Invalid; m_last.message = "constraint refers to missing geometry"; return m_last; }
        }
        for (const Geometry& g : m_geometry)
            for (Id r : { g.a, g.b, g.c }) if (r && !IsPoint(r)) { m_last = {}; m_last.status = SolveStatus::Invalid; m_last.message = "geometry refers to a missing point"; return m_last; }
        const std::vector<Geometry> before = m_geometry;
        SketchSolver solver(*this);
        m_last = solver.Run(maxIterations, tolerance);
        if (m_last.status != SolveStatus::Solved) m_geometry = before;   // a failed solve leaves the sketch as it was
        for (const Geometry& g : m_geometry)
            if (g.type == GeometryType::Circle && !(g.radius > 0.0)) { m_geometry = before; m_last.status = SolveStatus::Failed; m_last.message = "a circle radius would become zero or negative"; }
        return m_last;
    }

    Id Sketch::NearestPoint(double x, double y, double radius, Id ignore) const {
        Id best = 0;
        double bestD = radius;
        for (const Geometry& g : m_geometry) {
            if (g.type != GeometryType::Point || g.id == ignore) continue;
            const double d = Len(glm::dvec2(g.x - x, g.y - y));
            if (d <= bestD) { bestD = d; best = g.id; }
        }
        return best;
    }

    // ======================================================================== profiles
    namespace {
        std::vector<glm::dvec2> Polyline(const ProfileLoop& loop) {
            std::vector<glm::dvec2> pts;
            for (const ProfileSegment& s : loop.segments) {
                if (s.type == GeometryType::Line) { pts.push_back(s.start); continue; }
                double a0, sweep;
                if (s.type == GeometryType::Circle) { a0 = 0; sweep = 2 * PI; }
                else {
                    a0 = std::atan2(s.start.y - s.centre.y, s.start.x - s.centre.x);
                    double a1 = std::atan2(s.end.y - s.centre.y, s.end.x - s.centre.x);
                    sweep = a1 - a0;
                    if (s.counterClockwise) { while (sweep <= 0) sweep += 2 * PI; }
                    else { while (sweep >= 0) sweep -= 2 * PI; }
                }
                const int steps = std::max(4, int(std::fabs(sweep) / (2 * PI) * 64));
                for (int i = 0; i < steps; ++i) {
                    const double a = a0 + sweep * i / steps;
                    pts.push_back(s.centre + s.radius * glm::dvec2(std::cos(a), std::sin(a)));
                }
            }
            return pts;
        }
        bool Inside(const std::vector<glm::dvec2>& poly, glm::dvec2 p) {
            bool in = false;
            for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
                const glm::dvec2 a = poly[i], b = poly[j];
                if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
            }
            return in;
        }
        double SignedArea(const ProfileLoop& loop) {
            double area = 0;
            for (const ProfileSegment& s : loop.segments) {
                if (s.type == GeometryType::Circle) { area += PI * s.radius * s.radius; continue; }
                area += 0.5 * Cross(s.start, s.end);
                if (s.type == GeometryType::Arc) {
                    double sweep = std::atan2(s.end.y - s.centre.y, s.end.x - s.centre.x) - std::atan2(s.start.y - s.centre.y, s.start.x - s.centre.x);
                    if (s.counterClockwise) { while (sweep <= 0) sweep += 2 * PI; } else { while (sweep >= 0) sweep -= 2 * PI; }
                    area += 0.5 * s.radius * s.radius * (sweep - std::sin(sweep));
                }
            }
            return area;
        }
    }

    ProfileResult Sketch::Profiles(double tol) const {
        ProfileResult out;
        std::vector<ProfileLoop> loops;
        // nodes: endpoint positions merged within tolerance
        std::vector<glm::dvec2> nodes;
        auto node = [&](glm::dvec2 p) {
            for (size_t i = 0; i < nodes.size(); ++i) if (Len(nodes[i] - p) <= tol) return i;
            nodes.push_back(p);
            return nodes.size() - 1;
        };
        struct Edge { const Geometry* g; size_t n0, n1; bool used = false; };
        std::vector<Edge> edges;
        for (const Geometry& g : m_geometry) {
            if (g.construction) continue;
            if (g.type == GeometryType::Circle) {
                ProfileLoop loop;
                ProfileSegment s;
                s.type = GeometryType::Circle; s.centre = PointPosition(g.a); s.radius = g.radius; s.geometry = g.id;
                s.start = s.end = s.centre + glm::dvec2(g.radius, 0.0);
                loop.segments.push_back(s);
                loops.push_back(loop);
            }
            else if (g.type == GeometryType::Line || g.type == GeometryType::Arc) {
                const glm::dvec2 p0 = PointPosition(g.type == GeometryType::Line ? g.a : g.b), p1 = PointPosition(g.type == GeometryType::Line ? g.b : g.c);
                if (Len(p1 - p0) <= tol) { out.error = std::string(ToString(g.type)) + " " + std::to_string(g.id) + " has zero length"; return out; }
                edges.push_back({ &g, node(p0), node(p1) });
            }
        }
        std::vector<int> degree(nodes.size(), 0);
        for (const Edge& e : edges) { ++degree[e.n0]; ++degree[e.n1]; }
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (degree[i] == 1) { out.error = "open profile: an end at (" + std::to_string(nodes[i].x) + ", " + std::to_string(nodes[i].y) + ") is not connected"; return out; }
            if (degree[i] > 2) { out.error = "branching profile: " + std::to_string(degree[i]) + " curves meet at (" + std::to_string(nodes[i].x) + ", " + std::to_string(nodes[i].y) + ")"; return out; }
        }
        for (size_t start = 0; start < edges.size(); ++start) {
            if (edges[start].used) continue;
            ProfileLoop loop;
            size_t cur = start, at = edges[start].n0;
            while (true) {
                Edge& e = edges[cur];
                e.used = true;
                const bool forward = e.n0 == at;
                const size_t next = forward ? e.n1 : e.n0;
                ProfileSegment s;
                s.type = e.g->type; s.geometry = e.g->id;
                s.start = nodes[at]; s.end = nodes[next];
                if (e.g->type == GeometryType::Arc) { s.centre = PointPosition(e.g->a); s.radius = RadiusOf(e.g->id); s.counterClockwise = forward; }
                loop.segments.push_back(s);
                at = next;
                if (at == edges[start].n0) break;
                size_t found = edges.size();
                for (size_t k = 0; k < edges.size(); ++k) if (!edges[k].used && (edges[k].n0 == at || edges[k].n1 == at)) { found = k; break; }
                if (found == edges.size()) { out.error = "profile does not close"; return out; }
                cur = found;
            }
            loops.push_back(loop);
        }
        if (loops.empty()) { out.error = "the sketch has no closed profile"; return out; }
        for (ProfileLoop& l : loops) {
            l.signedArea = SignedArea(l);
            if (std::fabs(l.signedArea) <= tol * tol) { out.error = "a profile loop has no area"; return out; }
        }
        // nesting: depth = number of loops containing a loop; even depth = outer boundary, odd = hole
        std::vector<std::vector<glm::dvec2>> polys;
        for (const ProfileLoop& l : loops) polys.push_back(Polyline(l));
        const size_t k = loops.size();
        std::vector<int> depth(k, 0), parent(k, -1);
        for (size_t i = 0; i < k; ++i) {
            const glm::dvec2 probe = loops[i].segments[0].type == GeometryType::Circle ? loops[i].segments[0].start : 0.5 * (polys[i][0] + polys[i][polys[i].size() > 1 ? 1 : 0]);
            double bestArea = 0;
            for (size_t j = 0; j < k; ++j) {
                if (i == j || std::fabs(loops[j].signedArea) <= std::fabs(loops[i].signedArea)) continue;
                if (!Inside(polys[j], probe)) continue;
                ++depth[i];
                if (parent[i] < 0 || std::fabs(loops[j].signedArea) < bestArea) { parent[i] = int(j); bestArea = std::fabs(loops[j].signedArea); }
            }
        }
        std::map<size_t, size_t> profileOf;
        for (size_t i = 0; i < k; ++i) if (depth[i] % 2 == 0) { profileOf[i] = out.profiles.size(); out.profiles.push_back({ loops[i], {} }); }
        for (size_t i = 0; i < k; ++i) if (depth[i] % 2 == 1 && parent[i] >= 0) out.profiles[profileOf[size_t(parent[i])]].holes.push_back(loops[i]);
        return out;
    }

    // ======================================================================== JSON
    std::string Sketch::ToJson() const {
        json geo = json::array(), cons = json::array();
        for (const Geometry& g : m_geometry) {
            json j = { { "id", g.id }, { "type", ToString(g.type) } };
            if (g.construction) j["construction"] = true;
            switch (g.type) {
                case GeometryType::Point: j["x"] = g.x; j["y"] = g.y; if (g.fixed) j["fixed"] = true; break;
                case GeometryType::Line: j["a"] = g.a; j["b"] = g.b; break;
                case GeometryType::Circle: j["centre"] = g.a; j["radius"] = g.radius; break;
                case GeometryType::Arc: j["centre"] = g.a; j["start"] = g.b; j["end"] = g.c; break;
            }
            geo.push_back(j);
        }
        for (const Constraint& c : m_constraints) {
            json j = { { "id", c.id }, { "type", ToString(c.type) }, { "a", c.a } };
            if (c.b) j["b"] = c.b;
            if (c.c) j["c"] = c.c;
            if (IsDimension(c.type)) j["value"] = c.value;
            if (!c.expression.empty()) j["expression"] = c.expression;
            if (c.side) j["side"] = c.side;
            cons.push_back(j);
        }
        return json{ { "geometry", geo }, { "constraints", cons }, { "nextId", m_nextId } }.dump();
    }

    bool Sketch::FromJson(const std::string& text, Sketch& out, std::string* error) {
        try {
            const json j = json::parse(text);
            Sketch s;
            Id maxId = 0;
            for (const json& g : j.at("geometry")) {
                Geometry x;
                x.id = g.at("id").get<Id>();
                const std::string type = g.at("type").get<std::string>();
                x.construction = g.value("construction", false);
                if (type == "point") { x.type = GeometryType::Point; x.x = g.at("x").get<double>(); x.y = g.at("y").get<double>(); x.fixed = g.value("fixed", false); }
                else if (type == "line") { x.type = GeometryType::Line; x.a = g.at("a").get<Id>(); x.b = g.at("b").get<Id>(); }
                else if (type == "circle") { x.type = GeometryType::Circle; x.a = g.at("centre").get<Id>(); x.radius = g.at("radius").get<double>(); }
                else if (type == "arc") { x.type = GeometryType::Arc; x.a = g.at("centre").get<Id>(); x.b = g.at("start").get<Id>(); x.c = g.at("end").get<Id>(); }
                else throw std::runtime_error("unknown geometry type '" + type + "'");
                if (!x.id || s.Find(x.id)) throw std::runtime_error("duplicate or zero geometry id");
                if (!std::isfinite(x.x) || !std::isfinite(x.y) || !std::isfinite(x.radius)) throw std::runtime_error("non-finite geometry value");
                maxId = std::max(maxId, x.id);
                s.m_geometry.push_back(x);
            }
            for (const Geometry& g : s.m_geometry)
                for (Id r : { g.a, g.b, g.c }) if (r && !s.IsPoint(r)) throw std::runtime_error("geometry " + std::to_string(g.id) + " refers to a missing point");
            for (const json& c : j.at("constraints")) {
                Constraint x;
                x.id = c.at("id").get<Id>();
                if (!ParseConstraintType(c.at("type").get<std::string>(), x.type)) throw std::runtime_error("unknown constraint type '" + c.at("type").get<std::string>() + "'");
                x.a = c.at("a").get<Id>(); x.b = c.value("b", Id(0)); x.c = c.value("c", Id(0));
                x.value = c.value("value", 0.0);
                x.expression = c.value("expression", std::string());
                x.side = c.value("side", 0);
                for (Id r : { x.a, x.b, x.c }) if (r && !s.Find(r)) throw std::runtime_error("constraint " + std::to_string(x.id) + " refers to missing geometry");
                if (!x.id || s.FindConstraint(x.id)) throw std::runtime_error("duplicate or zero constraint id");
                maxId = std::max(maxId, x.id);
                s.m_constraints.push_back(x);
            }
            s.m_nextId = std::max<Id>(j.value("nextId", Id(1)), maxId + 1);
            out = std::move(s);
            return true;
        }
        catch (const std::exception& e) {
            if (error) *error = std::string("sketch: ") + e.what();
            return false;
        }
    }

    // ======================================================================== units
    double MetresPerUnit(Unit unit) {
        switch (unit) {
            case Unit::Millimetre: return 0.001;
            case Unit::Centimetre: return 0.01;
            case Unit::Metre: return 1.0;
            case Unit::Inch: return 0.0254;
        }
        return 1.0;
    }
    const char* UnitSymbol(Unit unit) {
        switch (unit) {
            case Unit::Millimetre: return "mm";
            case Unit::Centimetre: return "cm";
            case Unit::Metre: return "m";
            case Unit::Inch: return "in";
        }
        return "?";
    }
    bool ParseUnit(const char* text, Unit& out) {
        const std::string t = text ? text : "";
        if (t == "mm") out = Unit::Millimetre;
        else if (t == "cm") out = Unit::Centimetre;
        else if (t == "m") out = Unit::Metre;
        else if (t == "in") out = Unit::Inch;
        else return false;
        return true;
    }
}
