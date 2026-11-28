#pragma once
// Phisiks::Cad::Sketch: 2D geometry on a sketch plane, geometric / dimensional constraints and a
// numerical solver.
//
// Geometry: points, lines (two points), circles (centre point + radius), arcs (centre, start and end
// points; counter-clockwise from start to end; the solver keeps |start - centre| = |end - centre|).
// Lines and arcs share points, so a closed profile is a chain of geometry joined by shared points or
// by coincident constraints. Construction geometry takes part in constraints but not in profiles.
//
// Constraints are equations f(x) = 0 over the free parameters (point coordinates, circle radii).
// Solve() runs damped Gauss-Newton (Levenberg-Marquardt) from the current geometry, so an
// under-constrained sketch moves as little as possible. It then reports the degrees of freedom left
// (free parameters - rank of the constraint Jacobian), redundant constraints and failures.
// A dimension (distance, length, radius, diameter, angle, ...) has a value; a document may drive it
// from an expression over its parameters (see Document.h).
#include "Phisiks/Cad/CadApi.h"

#include <glm/vec2.hpp>

#include <string>
#include <vector>

namespace Phisiks::Cad {
    enum class GeometryType { Point, Line, Circle, Arc };
    PHISIKS_CAD_API const char* ToString(GeometryType type);

    struct Geometry {
        Id id = 0;
        GeometryType type = GeometryType::Point;
        bool construction = false;
        // Point: x, y. Circle: radius (+ centre in a). Others unused.
        double x = 0.0, y = 0.0, radius = 0.0;
        Id a = 0, b = 0, c = 0;      // Line: a, b. Circle: a = centre. Arc: a = centre, b = start, c = end.
        bool fixed = false;          // Point: does not move (Fix constraint)
    };

    enum class ConstraintType {
        Coincident,        // points a, b
        Horizontal,        // line a
        Vertical,          // line a
        Parallel,          // lines a, b
        Perpendicular,     // lines a, b
        Tangent,           // line a + circle / arc b, or circle / arc a + circle / arc b
        Equal,             // lines a, b (length) or circles / arcs a, b (radius)
        Concentric,        // circles / arcs a, b
        PointOnLine,       // point a, line b (infinite line)
        PointOnCircle,     // point a, circle / arc b
        Midpoint,          // point a, line b
        Symmetric,         // points a, b about line c
        Fix,               // point a
        // dimensions (value)
        Distance,          // points a, b
        HorizontalDistance,// points a, b: b.x - a.x
        VerticalDistance,  // points a, b: b.y - a.y
        Length,            // line a
        PointLineDistance, // point a, line b
        Radius,            // circle / arc a
        Diameter,          // circle / arc a
        Angle,             // lines a, b: angle from a to b, degrees
    };
    PHISIKS_CAD_API const char* ToString(ConstraintType type);
    PHISIKS_CAD_API bool ParseConstraintType(const std::string& text, ConstraintType& out);
    PHISIKS_CAD_API bool IsDimension(ConstraintType type);

    struct Constraint {
        Id id = 0;
        ConstraintType type = ConstraintType::Coincident;
        Id a = 0, b = 0, c = 0;
        double value = 0.0;          // dimensions: length in document units, angle in degrees
        std::string expression;      // optional: drives `value` from document parameters
        int side = 0;                // tangency / symmetric orientation chosen when created (+1 / -1)
    };

    enum class SolveStatus { Solved, Failed, Invalid };
    struct SolveResult {
        SolveStatus status = SolveStatus::Solved;
        int iterations = 0;
        double residual = 0.0;        // max |f(x)| at the end
        int freeParameters = 0;
        int equations = 0;
        int degreesOfFreedom = 0;     // freeParameters - rank(J); 0 = fully constrained
        std::vector<Id> redundant;    // constraints whose equations add no rank (redundant or conflicting)
        std::string message;
        bool FullyConstrained() const { return status == SolveStatus::Solved && degreesOfFreedom == 0; }
    };

    // A closed region for features: an outer loop and holes. Loops are polylines of segments.
    struct ProfileSegment {
        GeometryType type = GeometryType::Line;    // Line, Arc or Circle (full)
        glm::dvec2 start{ 0.0 }, end{ 0.0 }, centre{ 0.0 };
        double radius = 0.0;
        bool counterClockwise = true;              // arcs: direction from start to end along the loop
        Id geometry = 0;
    };
    struct ProfileLoop {
        std::vector<ProfileSegment> segments;
        double signedArea = 0.0;                   // > 0 counter-clockwise
    };
    struct Profile {
        ProfileLoop outer;
        std::vector<ProfileLoop> holes;
    };
    struct ProfileResult {
        std::vector<Profile> profiles;
        std::string error;                         // open chains, branches, self-touching loops
        bool ok() const { return error.empty(); }
    };

    class PHISIKS_CAD_API Sketch {
    public:
        // ---- geometry (returns the new id, 0 if the arguments are invalid)
        Id AddPoint(double x, double y, bool construction = false);
        Id AddLine(Id p1, Id p2, bool construction = false);
        Id AddLine(double x1, double y1, double x2, double y2, bool construction = false);
        Id AddCircle(Id centre, double radius, bool construction = false);
        Id AddCircleAt(double cx, double cy, double radius, bool construction = false);
        Id AddArc(Id centre, Id start, Id end, bool construction = false);
        Id AddArc(double cx, double cy, double radius, double startDegrees, double endDegrees, bool construction = false);
        // Four lines sharing corner points, with horizontal / vertical constraints. Returns the line ids.
        std::vector<Id> AddRectangle(double x1, double y1, double x2, double y2);
        // Removes geometry and every constraint / geometry that depends on it (a line's points stay).
        bool Remove(Id geometry);

        const Geometry* Find(Id id) const;
        Geometry* Find(Id id);
        const std::vector<Geometry>& Geometries() const { return m_geometry; }
        bool SetPoint(Id point, double x, double y);          // moves a point (then Solve())
        bool SetRadius(Id circle, double radius);
        bool SetConstruction(Id geometry, bool construction);
        glm::dvec2 PointPosition(Id point) const;
        double RadiusOf(Id circleOrArc) const;

        // ---- constraints (returns the id, 0 if the arguments do not fit the type)
        Id AddConstraint(ConstraintType type, Id a, Id b = 0, Id c = 0, double value = 0.0);
        // A dimension with the current measured value (so adding it does not move anything).
        Id AddDimension(ConstraintType type, Id a, Id b = 0);
        bool RemoveConstraint(Id constraint);
        bool SetConstraintValue(Id constraint, double value);
        bool SetConstraintExpression(Id constraint, const std::string& expression);
        const Constraint* FindConstraint(Id id) const;
        const std::vector<Constraint>& Constraints() const { return m_constraints; }
        // The value the geometry has now for a dimension type (distance, angle, ...).
        double Measure(ConstraintType type, Id a, Id b = 0) const;

        // ---- solving and analysis
        SolveResult Solve(int maxIterations = 100, double tolerance = 1e-9);
        const SolveResult& LastSolve() const { return m_last; }
        ProfileResult Profiles(double tolerance = 1e-6) const;

        // Nearest point (within `radius`) to snap to, 0 if none.
        Id NearestPoint(double x, double y, double radius, Id ignore = 0) const;

        // ---- serialization (JSON object text: {"geometry":[...],"constraints":[...]})
        std::string ToJson() const;
        static bool FromJson(const std::string& text, Sketch& out, std::string* error = nullptr);

        Id NextId() const { return m_nextId; }

    private:
        friend struct SketchSolver;
        Id NewId() { return m_nextId++; }
        bool IsPoint(Id id) const;
        bool IsLine(Id id) const;
        bool IsRound(Id id) const;    // circle or arc
        std::vector<Geometry> m_geometry;
        std::vector<Constraint> m_constraints;
        SolveResult m_last;
        Id m_nextId = 1;
    };
}
