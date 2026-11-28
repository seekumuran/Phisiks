#pragma once
// Phisiks::Cad::Document: a parametric part. Named parameters (expressions), and an ordered feature
// history that is regenerated from the start whenever anything changes:
//
//   Sketch        2D sketch (Sketch.h) on a plane (XY / XZ / YZ or a custom frame) with an offset
//   Extrude       sketch profiles swept along the plane normal (distance, symmetric, reverse)
//   Revolve       sketch profiles revolved about the sketch x / y axis or a sketch line (angle)
//   Box / Cylinder / Sphere   primitives
//   Fillet / Chamfer          edges of a body (selected by position, see EdgeRef) or all edges
//   Boolean       union / subtract / intersect of two bodies (the tool body is consumed)
//   LinearPattern / CircularPattern   repeats a feature's tool (e.g. a cut) or a whole body
//   Mirror        a body about a plane (optionally keeping the original)
//   Import        exact geometry from a STEP / IGES / BREP file (a project asset), kept in the
//                 document as BREP so it reopens without the file
//
// Extrude / Revolve / primitives / Import create a new body or combine with a target body (join,
// cut, intersect). Every numeric field is an expression over the parameters ("width / 2"). Feature
// order is the dependency order: a feature can only use sketches and bodies created before it.
// Regenerate() reports an error per feature; later features that depend on a failed one are
// skipped, independent ones still build. All lengths are in the document's unit.
#include "Phisiks/Cad/CadApi.h"
#include "Phisiks/Cad/Kernel.h"
#include "Phisiks/Cad/Sketch.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Phisiks::Cad {
    enum class FeatureType { Sketch, Extrude, Revolve, Box, Cylinder, Sphere, Fillet, Chamfer, Boolean, LinearPattern, CircularPattern, Mirror, Import };
    PHISIKS_CAD_API const char* ToString(FeatureType type);
    PHISIKS_CAD_API bool ParseFeatureType(const std::string& text, FeatureType& out);

    enum class FeatureOp { NewBody, Join, Cut, Intersect };
    PHISIKS_CAD_API const char* ToString(FeatureOp op);

    // An edge chosen by where it is (midpoint, direction) on the body the feature modifies. When the
    // body regenerates the nearest edge with the same direction is used; if none is close enough the
    // feature fails with "edge reference lost" rather than picking an arbitrary edge.
    struct EdgeRef {
        glm::dvec3 midpoint{ 0.0 };
        glm::dvec3 tangent{ 0.0 };
    };

    struct Parameter {
        std::string name;
        std::string expression;     // "40", "width / 2"
        double value = 0.0;         // evaluated by Regenerate
        std::string error;
    };

    struct Feature {
        Id id = 0;
        FeatureType type = FeatureType::Sketch;
        std::string name;
        bool suppressed = false;
        // Numeric fields as expressions, by key:
        //   Sketch: offset   Extrude: distance   Revolve: angle   Box: x y z   Cylinder: radius height
        //   Sphere: radius   Fillet: radius   Chamfer: distance   LinearPattern: count spacing
        //   CircularPattern: count angle
        std::map<std::string, std::string> values;
        // Sketch
        Sketch sketch;
        std::string plane = "XY";   // XY, XZ, YZ, custom
        Plane frame;                // custom plane
        // Extrude / Revolve
        Id profileSketch = 0;
        bool symmetric = false, reverse = false;
        std::string axis = "y";     // Revolve: "x", "y" (sketch axes) or "line" (axisLine)
        Id axisLine = 0;
        // Body creation / combination
        FeatureOp op = FeatureOp::NewBody;
        Id body = 0;                // target body (0 = the latest body)
        // Primitives
        glm::dvec3 position{ 0.0 };
        glm::dvec3 direction{ 0.0, 0.0, 1.0 };   // Cylinder axis, LinearPattern direction, CircularPattern / Mirror axis or normal
        glm::dvec3 origin{ 0.0 };                // CircularPattern axis origin, Mirror plane origin
        // Fillet / Chamfer
        std::vector<EdgeRef> edges;              // empty = every edge
        // Boolean
        BooleanOp boolean = BooleanOp::Union;
        Id tool = 0;                             // tool body
        // Pattern: repeat this feature's tool (0 = pattern the whole target body); Mirror
        Id source = 0;
        bool keepOriginal = true;
        // Import
        std::string asset;                       // asset reference (asset:<id>/<name>)
        std::string brep;                        // exact geometry cache
    };

    struct FeatureState {
        bool built = false;
        bool skipped = false;       // depends on a failed feature
        std::string error;
        double milliseconds = 0.0;
        SolveResult solve;          // sketches
        Properties properties;      // the body after the feature
    };

    struct Body {
        Id id = 0;                  // the feature that created it
        std::string name;
        Shape shape;
        Id lastFeature = 0;
    };

    struct RegenerateResult {
        bool ok = true;
        int built = 0, failed = 0, skipped = 0;
        double milliseconds = 0.0;
        std::string firstError;
    };

    // Resolves an Import feature's asset reference to a file path ("" if unknown).
    using AssetResolver = std::function<std::string(const std::string& reference)>;

    class PHISIKS_CAD_API Document {
    public:
        Unit unit = Unit::Millimetre;

        // ---- parameters
        bool SetParameter(const std::string& name, const std::string& expression, std::string* error = nullptr);   // adds or changes
        bool RemoveParameter(const std::string& name, std::string* error = nullptr);                                // refused while used
        const std::vector<Parameter>& Parameters() const { return m_parameters; }
        bool ParameterValue(const std::string& name, double& value) const;

        // ---- features
        Id AddFeature(Feature feature);                  // appended; fills id / name; returns the id
        Id InsertFeature(Feature feature, size_t index); // at a history position (dependencies checked)
        bool UpdateFeature(const Feature& feature, std::string* error = nullptr);
        bool RemoveFeature(Id id, std::string* error = nullptr);          // refused while other features use it
        bool MoveFeature(Id id, size_t newIndex, std::string* error = nullptr);
        const Feature* FindFeature(Id id) const;
        Feature* FindFeature(Id id);
        const std::vector<Feature>& Features() const { return m_features; }
        std::vector<Id> Dependencies(const Feature& feature) const;        // sketches / bodies / features it uses
        std::vector<Id> Dependents(Id id) const;
        size_t IndexOf(Id id) const;                                       // npos if absent

        // Builds features [0, rollback) (rollback = npos: every feature).
        RegenerateResult Regenerate(const AssetResolver& resolver = {});
        size_t rollback = size_t(-1);
        const FeatureState* State(Id feature) const;
        const std::vector<Body>& Bodies() const { return m_bodies; }
        const Body* FindBody(Id id) const;
        Shape Result() const;                                              // every body, as one compound
        // The shape a feature produced (the body after it), for edge picking while editing it.
        Shape ShapeAfter(Id feature) const;
        Shape ShapeBefore(Id feature) const;                               // the body a modifying feature starts from

        // Edge selection helpers (see EdgeRef).
        static EdgeRef MakeEdgeRef(const Shape& shape, int edgeIndex);
        static bool ResolveEdges(const Shape& shape, const std::vector<EdgeRef>& refs, std::vector<int>& indices, std::string* error);

        // Exchange in the document's unit. STEP / IGES / BREP are read and written in millimetres
        // (the convention of those files here) and scaled to / from the document unit.
        static Shape ImportFile(const std::string& path, Unit unit, std::string* error = nullptr);
        bool ExportResult(const std::string& path, FileFormat format, std::string* error = nullptr) const;
        static bool ExportShape(const Shape& shape, Unit unit, const std::string& path, FileFormat format, std::string* error = nullptr);

        // Evaluates one expression with the document's parameters.
        bool Eval(const std::string& expression, double& out, std::string* error = nullptr) const;

        // ---- persistence: {"format":"phisiks-cad","version":1,...} (also the .phcad file format)
        std::string ToJson(bool pretty = false) const;
        static bool FromJson(const std::string& text, Document& out, std::string* error = nullptr);
        static constexpr int FORMAT_VERSION = 1;

    private:
        bool EvaluateParameters(std::string* error);
        std::vector<Parameter> m_parameters;
        std::vector<Feature> m_features;
        std::map<Id, FeatureState> m_state;
        std::map<Id, Shape> m_after, m_before, m_tools;
        std::vector<Body> m_bodies;
        Id m_nextId = 1;
    };
}
