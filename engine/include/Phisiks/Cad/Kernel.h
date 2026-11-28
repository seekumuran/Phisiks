#pragma once
// Phisiks::Cad::Kernel: the PHISIKS-owned interface to the boundary-representation (B-rep) geometry
// kernel. The implementation uses Open CASCADE Technology (OCCT); nothing outside PhisiksCad sees
// OCCT types. A Shape is an immutable, reference-counted solid (or compound of solids), so copying is
// cheap and a document can keep the result of every feature.
//
// All lengths are in the calling document's unit; the kernel itself is unit-less. Functions return
// a null Shape and set `error` on failure (invalid input, the kernel could not build the result, or
// the result failed the validity check). When PHISIKS is built without a kernel (PHISIKS_CAD_KERNEL
// =None) every modelling function fails with "no geometry kernel"; sketches and documents still work.
#include "Phisiks/Cad/CadApi.h"
#include "Phisiks/Cad/Sketch.h"

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <memory>
#include <string>
#include <vector>

namespace Phisiks::Cad {
    struct ShapeData;   // kernel-private

    class PHISIKS_CAD_API Shape {
    public:
        Shape() = default;
        bool IsNull() const { return !m_data; }
        explicit operator bool() const { return m_data != nullptr; }
        const std::shared_ptr<const ShapeData>& Data() const { return m_data; }
        explicit Shape(std::shared_ptr<const ShapeData> data) : m_data(std::move(data)) {}
    private:
        std::shared_ptr<const ShapeData> m_data;
    };

    // A sketch plane: origin and orthonormal axes (u = sketch x, v = sketch y, normal = u x v).
    struct Plane {
        glm::dvec3 origin{ 0.0 };
        glm::dvec3 u{ 1.0, 0.0, 0.0 };
        glm::dvec3 v{ 0.0, 1.0, 0.0 };
        glm::dvec3 Normal() const { return glm::normalize(glm::cross(u, v)); }
        glm::dvec3 ToWorld(glm::dvec2 p) const { return origin + u * p.x + v * p.y; }
        static Plane XY() { return {}; }
        static Plane XZ() { return { glm::dvec3(0.0), glm::dvec3(1.0, 0.0, 0.0), glm::dvec3(0.0, 0.0, -1.0) }; }   // normal +Y
        static Plane YZ() { return { glm::dvec3(0.0), glm::dvec3(0.0, 1.0, 0.0), glm::dvec3(0.0, 0.0, 1.0) }; }    // normal +X
    };

    enum class BooleanOp { Union, Subtract, Intersect };
    enum class FileFormat { Step, Iges, Stl, Brep };
    PHISIKS_CAD_API bool FormatFromPath(const std::string& path, FileFormat& out);

    enum class SurfaceType { Plane, Cylinder, Cone, Sphere, Torus, BSpline, Other };
    enum class CurveType { Line, Circle, Ellipse, BSpline, Other };
    PHISIKS_CAD_API const char* ToString(SurfaceType type);
    PHISIKS_CAD_API const char* ToString(CurveType type);

    struct FaceInfo {
        int index = 0;                 // 1-based, stable for one Shape
        SurfaceType surface = SurfaceType::Other;
        double area = 0.0;
        glm::dvec3 centroid{ 0.0 };
        glm::dvec3 normal{ 0.0 };      // at the centroid's projection (planes: the plane normal)
        double radius = 0.0;           // cylinders, cones (reference), spheres, tori (major)
    };
    struct EdgeInfo {
        int index = 0;                 // 1-based
        CurveType curve = CurveType::Other;
        double length = 0.0;
        glm::dvec3 midpoint{ 0.0 };
        glm::dvec3 tangent{ 0.0 };     // at the midpoint, unit
        glm::dvec3 start{ 0.0 }, end{ 0.0 };
        double radius = 0.0;           // circles
    };
    struct Properties {
        double volume = 0.0, area = 0.0;
        glm::dvec3 centroid{ 0.0 };
        glm::dvec3 boundsMin{ 0.0 }, boundsMax{ 0.0 };
        int solids = 0, faces = 0, edges = 0, vertices = 0;
        bool valid = false;            // passes the kernel's topology / geometry check
    };

    // Triangles for display and picking. faceOf[t] is the FaceInfo index of triangle t; edges are
    // polylines with their EdgeInfo index.
    struct Tessellation {
        std::vector<glm::vec3> positions, normals;
        std::vector<uint32_t> indices;
        std::vector<int> faceOf;
        struct Polyline { int edge = 0; std::vector<glm::vec3> points; };
        std::vector<Polyline> edges;
    };

    namespace Kernel {
        PHISIKS_CAD_API bool Available();
        PHISIKS_CAD_API std::string Name();        // "Open CASCADE Technology 7.9.3" or "none"

        // ---- construction
        // A planar face from a sketch profile (outer loop minus holes).
        PHISIKS_CAD_API Shape Face(const Profile& profile, const Plane& plane, std::string* error = nullptr);
        PHISIKS_CAD_API Shape Faces(const std::vector<Profile>& profiles, const Plane& plane, std::string* error = nullptr);
        // Sweeps faces along `direction` (its length is the distance); `symmetric` extrudes half each way.
        PHISIKS_CAD_API Shape Extrude(const Shape& faces, const glm::dvec3& direction, bool symmetric, std::string* error = nullptr);
        PHISIKS_CAD_API Shape Revolve(const Shape& faces, const glm::dvec3& axisOrigin, const glm::dvec3& axisDirection, double angleDegrees, std::string* error = nullptr);
        PHISIKS_CAD_API Shape Box(const glm::dvec3& min, const glm::dvec3& size, std::string* error = nullptr);
        PHISIKS_CAD_API Shape Cylinder(const glm::dvec3& baseCentre, const glm::dvec3& axis, double radius, double height, std::string* error = nullptr);
        PHISIKS_CAD_API Shape Sphere(const glm::dvec3& centre, double radius, std::string* error = nullptr);

        // ---- modification
        PHISIKS_CAD_API Shape Boolean(const Shape& a, const Shape& b, BooleanOp op, std::string* error = nullptr);
        // Edge indices refer to Edges(shape). An empty list means every edge.
        PHISIKS_CAD_API Shape Fillet(const Shape& shape, const std::vector<int>& edges, double radius, std::string* error = nullptr);
        PHISIKS_CAD_API Shape Chamfer(const Shape& shape, const std::vector<int>& edges, double distance, std::string* error = nullptr);
        PHISIKS_CAD_API Shape Transform(const Shape& shape, const glm::dmat4& matrix, std::string* error = nullptr);
        // `count` copies (including the original) fused into one shape.
        PHISIKS_CAD_API Shape LinearPattern(const Shape& shape, const glm::dvec3& step, int count, std::string* error = nullptr);
        PHISIKS_CAD_API Shape CircularPattern(const Shape& shape, const glm::dvec3& axisOrigin, const glm::dvec3& axisDirection, int count, double totalDegrees, std::string* error = nullptr);
        PHISIKS_CAD_API Shape Mirror(const Shape& shape, const glm::dvec3& planeOrigin, const glm::dvec3& planeNormal, bool keepOriginal, std::string* error = nullptr);
        PHISIKS_CAD_API Shape Compound(const std::vector<Shape>& shapes);

        // ---- queries
        PHISIKS_CAD_API Properties Measure(const Shape& shape);
        PHISIKS_CAD_API std::vector<FaceInfo> FacesOf(const Shape& shape);
        PHISIKS_CAD_API std::vector<EdgeInfo> EdgesOf(const Shape& shape);
        // One face / edge of a shape (index from FacesOf / EdgesOf) as a shape of its own (measuring).
        PHISIKS_CAD_API Shape FaceShape(const Shape& shape, int faceIndex);
        PHISIKS_CAD_API Shape EdgeShape(const Shape& shape, int edgeIndex);
        // Closest distance between two shapes (0 if they touch or overlap), with the closest points.
        PHISIKS_CAD_API bool Distance(const Shape& a, const Shape& b, double& distance, glm::dvec3* pointA = nullptr, glm::dvec3* pointB = nullptr);
        PHISIKS_CAD_API bool DistanceToPoint(const Shape& shape, const glm::dvec3& point, double& distance, glm::dvec3* closest = nullptr);
        // linearDeflection: max chord error (document units); angularDeflection: radians.
        PHISIKS_CAD_API Tessellation Tessellate(const Shape& shape, double linearDeflection, double angularDeflection = 0.35);

        // ---- exchange
        // STEP / IGES / BREP keep exact geometry; STL is triangles (the import is a faceted solid
        // or shell, not an editable parametric model).
        PHISIKS_CAD_API Shape Import(const std::string& path, FileFormat format, std::string* error = nullptr);
        PHISIKS_CAD_API bool Export(const Shape& shape, const std::string& path, FileFormat format, std::string* error = nullptr, double stlDeflection = 0.1);
        // Exact geometry as text (OCCT BREP), for embedding an imported shape in a document.
        PHISIKS_CAD_API std::string ToBrep(const Shape& shape);
        PHISIKS_CAD_API Shape FromBrep(const std::string& text, std::string* error = nullptr);
    }
}
