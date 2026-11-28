// Phisiks::Cad::Kernel implemented with Open CASCADE Technology (LGPL-2.1 + OCCT exception, linked
// dynamically; see THIRD_PARTY_NOTICES.md). Built when PHISIKS_CAD_HAS_OCCT is defined; otherwise
// every modelling call reports "no geometry kernel" (KernelNone at the end of this file).
#include "Phisiks/Cad/Kernel.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#if defined(PHISIKS_CAD_HAS_OCCT)
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepLib_ToolTriangulatedShape.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GProp_GProps.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <IGESControl_Controller.hxx>
#include <IGESControl_Reader.hxx>
#include <IGESControl_Writer.hxx>
#include <Interface_Static.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_PrinterOStream.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Controller.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Version.hxx>
#include <StlAPI.hxx>
#include <StlAPI_Writer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#endif

namespace Phisiks::Cad {
    const char* ToString(SurfaceType t) {
        switch (t) {
            case SurfaceType::Plane: return "plane"; case SurfaceType::Cylinder: return "cylinder"; case SurfaceType::Cone: return "cone";
            case SurfaceType::Sphere: return "sphere"; case SurfaceType::Torus: return "torus"; case SurfaceType::BSpline: return "bspline";
            case SurfaceType::Other: return "other";
        }
        return "?";
    }
    const char* ToString(CurveType t) {
        switch (t) {
            case CurveType::Line: return "line"; case CurveType::Circle: return "circle"; case CurveType::Ellipse: return "ellipse";
            case CurveType::BSpline: return "bspline"; case CurveType::Other: return "other";
        }
        return "?";
    }
    bool FormatFromPath(const std::string& path, FileFormat& out) {
        std::string ext = path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.'));
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext == ".step" || ext == ".stp") out = FileFormat::Step;
        else if (ext == ".iges" || ext == ".igs") out = FileFormat::Iges;
        else if (ext == ".stl") out = FileFormat::Stl;
        else if (ext == ".brep" || ext == ".brp") out = FileFormat::Brep;
        else return false;
        return true;
    }

#if defined(PHISIKS_CAD_HAS_OCCT)
    struct ShapeData {
        TopoDS_Shape shape;
    };

    namespace {
        // OCCT's data-exchange toolkits print transfer statistics to stdout; PHISIKS reports results
        // itself (and stdout may carry the agent-interface protocol), so the console printer goes.
        void QuietKernel() {
            static const bool done = [] {
                const Handle(Message_Messenger)& m = Message::DefaultMessenger();
                m->RemovePrinters(STANDARD_TYPE(Message_PrinterOStream));
                return true;
            }();
            (void)done;
        }
        Shape Wrap(const TopoDS_Shape& s) {
            if (s.IsNull()) return {};
            auto d = std::make_shared<ShapeData>();
            d->shape = s;
            return Shape(std::move(d));
        }
        const TopoDS_Shape& S(const Shape& s) { static const TopoDS_Shape empty; return s ? s.Data()->shape : empty; }
        gp_Pnt P(const glm::dvec3& v) { return gp_Pnt(v.x, v.y, v.z); }
        gp_Dir D(const glm::dvec3& v) { return gp_Dir(v.x, v.y, v.z); }
        glm::dvec3 G(const gp_Pnt& p) { return { p.X(), p.Y(), p.Z() }; }
        glm::dvec3 G(const gp_Vec& v) { return { v.X(), v.Y(), v.Z() }; }
        void SetError(std::string* error, const std::string& m) { if (error) *error = m; }
        std::string FailureText(const Standard_Failure& f) {
            const char* m = f.GetMessageString();
            return std::string("geometry kernel: ") + (m && *m ? m : f.DynamicType()->Name());
        }
        bool Valid(const TopoDS_Shape& s) { return !s.IsNull() && BRepCheck_Analyzer(s).IsValid(); }

        // Runs a kernel operation, converting kernel exceptions and invalid results into errors.
        template<class F> Shape Guard(std::string* error, const char* what, F&& f) {
            try {
                TopoDS_Shape s = f();
                if (s.IsNull()) { SetError(error, std::string(what) + ": no result"); return {}; }
                if (!Valid(s)) { SetError(error, std::string(what) + ": the result is not a valid solid (check sizes: a fillet or chamfer may be too large, or geometry may self-intersect)"); return {}; }
                return Wrap(s);
            }
            catch (const Standard_Failure& e) { SetError(error, std::string(what) + ": " + FailureText(e)); }
            catch (const std::exception& e) { SetError(error, std::string(what) + ": " + e.what()); }
            return {};
        }
        TopoDS_Shape Unify(const TopoDS_Shape& s) {
            try {
                ShapeUpgrade_UnifySameDomain u(s, true, true, true);
                u.Build();
                return u.Shape().IsNull() ? s : u.Shape();
            }
            catch (...) { return s; }
        }
        TopoDS_Shape FuseAll(const std::vector<TopoDS_Shape>& shapes) {
            if (shapes.size() == 1) return shapes[0];
            TopTools_ListOfShape args, tools;
            args.Append(shapes[0]);
            for (size_t i = 1; i < shapes.size(); ++i) tools.Append(shapes[i]);
            BRepAlgoAPI_Fuse fuse;
            fuse.SetArguments(args);
            fuse.SetTools(tools);
            fuse.Build();
            if (!fuse.IsDone() || fuse.HasErrors()) throw std::runtime_error("fusing the copies failed");
            return Unify(fuse.Shape());
        }
        gp_Trsf Trsf(const glm::dmat4& m) {
            gp_Trsf t;
            t.SetValues(m[0][0], m[1][0], m[2][0], m[3][0],
                        m[0][1], m[1][1], m[2][1], m[3][1],
                        m[0][2], m[1][2], m[2][2], m[3][2]);
            return t;
        }
        TopoDS_Shape Moved(const TopoDS_Shape& s, const gp_Trsf& t) { return BRepBuilderAPI_Transform(s, t, true).Shape(); }

        TopoDS_Wire MakeWire(const ProfileLoop& loop, const Plane& plane) {
            BRepBuilderAPI_MakeWire wire;
            const gp_Dir n = D(plane.Normal());
            for (const ProfileSegment& s : loop.segments) {
                if (s.type == GeometryType::Line) {
                    wire.Add(BRepBuilderAPI_MakeEdge(P(plane.ToWorld(s.start)), P(plane.ToWorld(s.end))).Edge());
                }
                else if (s.type == GeometryType::Circle) {
                    gp_Circ c(gp_Ax2(P(plane.ToWorld(s.centre)), n, D(plane.u)), s.radius);
                    wire.Add(BRepBuilderAPI_MakeEdge(c).Edge());
                }
                else {
                    // arc through its midpoint in the loop's direction
                    const double a0 = std::atan2(s.start.y - s.centre.y, s.start.x - s.centre.x);
                    double sweep = std::atan2(s.end.y - s.centre.y, s.end.x - s.centre.x) - a0;
                    const double TWO_PI = 6.283185307179586;
                    if (s.counterClockwise) { while (sweep <= 0) sweep += TWO_PI; } else { while (sweep >= 0) sweep -= TWO_PI; }
                    const double am = a0 + 0.5 * sweep;
                    const glm::dvec2 mid = s.centre + s.radius * glm::dvec2(std::cos(am), std::sin(am));
                    GC_MakeArcOfCircle arc(P(plane.ToWorld(s.start)), P(plane.ToWorld(mid)), P(plane.ToWorld(s.end)));
                    if (!arc.IsDone()) throw std::runtime_error("an arc of the profile is degenerate");
                    wire.Add(BRepBuilderAPI_MakeEdge(arc.Value()).Edge());
                }
                if (!wire.IsDone() && wire.Error() == BRepBuilderAPI_DisconnectedWire) throw std::runtime_error("the profile loop is not connected");
            }
            if (!wire.IsDone()) throw std::runtime_error("the profile loop could not be built");
            return wire.Wire();
        }
        TopoDS_Shape MakeFace(const Profile& profile, const Plane& plane) {
            const gp_Pln pln(gp_Ax3(P(plane.origin), D(plane.Normal()), D(plane.u)));
            BRepBuilderAPI_MakeFace outer(pln, MakeWire(profile.outer, plane), true);
            if (!outer.IsDone()) throw std::runtime_error("the outer loop does not make a face");
            TopoDS_Shape face = outer.Face();
            for (const ProfileLoop& hole : profile.holes) {
                BRepBuilderAPI_MakeFace h(pln, MakeWire(hole, plane), true);
                if (!h.IsDone()) throw std::runtime_error("a hole loop does not make a face");
                BRepAlgoAPI_Cut cut(face, h.Face());
                if (!cut.IsDone() || cut.HasErrors()) throw std::runtime_error("subtracting a hole failed");
                face = cut.Shape();
            }
            return face;
        }
        // Edges usable by fillets / chamfers: not degenerate, not a seam.
        bool FilletableEdge(const TopoDS_Edge& e, const TopTools_IndexedDataMapOfShapeListOfShape& edgeFaces) {
            if (BRep_Tool::Degenerated(e)) return false;
            const int i = edgeFaces.FindIndex(e);
            if (i == 0) return false;
            const TopTools_ListOfShape& faces = edgeFaces.FindFromIndex(i);
            if (faces.Extent() < 2) return false;
            const TopoDS_Shape& f0 = faces.First();
            for (const TopoDS_Shape& f : faces) if (!f.IsSame(f0)) return true;
            return false;   // the same face on both sides: a seam
        }
    }

    namespace Kernel {
        bool Available() { return true; }
        std::string Name() { return std::string("Open CASCADE Technology ") + OCC_VERSION_COMPLETE; }

        Shape Face(const Profile& profile, const Plane& plane, std::string* error) {
            return Faces({ profile }, plane, error);
        }
        Shape Faces(const std::vector<Profile>& profiles, const Plane& plane, std::string* error) {
            if (profiles.empty()) { SetError(error, "no profile"); return {}; }
            try {
                if (profiles.size() == 1) return Wrap(MakeFace(profiles[0], plane));
                BRep_Builder b;
                TopoDS_Compound c;
                b.MakeCompound(c);
                for (const Profile& p : profiles) b.Add(c, MakeFace(p, plane));
                return Wrap(c);
            }
            catch (const Standard_Failure& e) { SetError(error, FailureText(e)); }
            catch (const std::exception& e) { SetError(error, e.what()); }
            return {};
        }
        Shape Extrude(const Shape& faces, const glm::dvec3& direction, bool symmetric, std::string* error) {
            if (!faces || !(glm::length(direction) > 1e-12)) { SetError(error, "nothing to extrude"); return {}; }
            return Guard(error, "extrude", [&] {
                TopoDS_Shape base = S(faces);
                if (symmetric) { gp_Trsf t; t.SetTranslation(gp_Vec(-0.5 * direction.x, -0.5 * direction.y, -0.5 * direction.z)); base = Moved(base, t); }
                BRepPrimAPI_MakePrism prism(base, gp_Vec(direction.x, direction.y, direction.z), true);
                prism.Build();
                return prism.IsDone() ? prism.Shape() : TopoDS_Shape();
            });
        }
        Shape Revolve(const Shape& faces, const glm::dvec3& origin, const glm::dvec3& axis, double angleDegrees, std::string* error) {
            if (!faces || !(glm::length(axis) > 1e-12)) { SetError(error, "nothing to revolve, or no axis"); return {}; }
            return Guard(error, "revolve", [&] {
                BRepPrimAPI_MakeRevol revol(S(faces), gp_Ax1(P(origin), D(axis)), angleDegrees * 3.14159265358979323846 / 180.0, true);
                revol.Build();
                return revol.IsDone() ? revol.Shape() : TopoDS_Shape();
            });
        }
        Shape Box(const glm::dvec3& min, const glm::dvec3& size, std::string* error) {
            return Guard(error, "box", [&] { return BRepPrimAPI_MakeBox(P(min), size.x, size.y, size.z).Shape(); });
        }
        Shape Cylinder(const glm::dvec3& base, const glm::dvec3& axis, double radius, double height, std::string* error) {
            if (!(glm::length(axis) > 1e-12)) { SetError(error, "no axis"); return {}; }
            return Guard(error, "cylinder", [&] { return BRepPrimAPI_MakeCylinder(gp_Ax2(P(base), D(axis)), radius, height).Shape(); });
        }
        Shape Sphere(const glm::dvec3& centre, double radius, std::string* error) {
            return Guard(error, "sphere", [&] { return BRepPrimAPI_MakeSphere(P(centre), radius).Shape(); });
        }

        Shape Boolean(const Shape& a, const Shape& b, BooleanOp op, std::string* error) {
            if (!a || !b) { SetError(error, "missing operand"); return {}; }
            return Guard(error, op == BooleanOp::Union ? "union" : op == BooleanOp::Subtract ? "subtract" : "intersect", [&]() -> TopoDS_Shape {
                TopoDS_Shape r;
                if (op == BooleanOp::Union) { BRepAlgoAPI_Fuse f(S(a), S(b)); if (!f.IsDone() || f.HasErrors()) return {}; r = f.Shape(); }
                else if (op == BooleanOp::Subtract) { BRepAlgoAPI_Cut f(S(a), S(b)); if (!f.IsDone() || f.HasErrors()) return {}; r = f.Shape(); }
                else { BRepAlgoAPI_Common f(S(a), S(b)); if (!f.IsDone() || f.HasErrors()) return {}; r = f.Shape(); }
                TopTools_IndexedMapOfShape solids;
                TopExp::MapShapes(r, TopAbs_SOLID, solids);
                if (solids.Extent() == 0) throw std::runtime_error(op == BooleanOp::Intersect ? "the bodies do not overlap" : "nothing is left");
                return Unify(r);
            });
        }
        Shape Fillet(const Shape& shape, const std::vector<int>& edges, double radius, std::string* error) {
            if (!shape) { SetError(error, "no body"); return {}; }
            return Guard(error, "fillet", [&]() -> TopoDS_Shape {
                TopTools_IndexedMapOfShape map;
                TopExp::MapShapes(S(shape), TopAbs_EDGE, map);
                TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
                TopExp::MapShapesAndAncestors(S(shape), TopAbs_EDGE, TopAbs_FACE, edgeFaces);
                BRepFilletAPI_MakeFillet mk(S(shape));
                int added = 0;
                if (edges.empty()) { for (int i = 1; i <= map.Extent(); ++i) if (FilletableEdge(TopoDS::Edge(map(i)), edgeFaces)) { mk.Add(radius, TopoDS::Edge(map(i))); ++added; } }
                else for (int i : edges) {
                    if (i < 1 || i > map.Extent()) throw std::runtime_error("edge " + std::to_string(i) + " does not exist");
                    mk.Add(radius, TopoDS::Edge(map(i)));
                    ++added;
                }
                if (!added) throw std::runtime_error("no edges to fillet");
                mk.Build();
                if (!mk.IsDone()) throw std::runtime_error("the radius may be too large for the selected edges");
                return mk.Shape();
            });
        }
        Shape Chamfer(const Shape& shape, const std::vector<int>& edges, double distance, std::string* error) {
            if (!shape) { SetError(error, "no body"); return {}; }
            return Guard(error, "chamfer", [&]() -> TopoDS_Shape {
                TopTools_IndexedMapOfShape map;
                TopExp::MapShapes(S(shape), TopAbs_EDGE, map);
                TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
                TopExp::MapShapesAndAncestors(S(shape), TopAbs_EDGE, TopAbs_FACE, edgeFaces);
                BRepFilletAPI_MakeChamfer mk(S(shape));
                int added = 0;
                auto add = [&](int i) { mk.Add(distance, TopoDS::Edge(map(i))); ++added; };
                if (edges.empty()) { for (int i = 1; i <= map.Extent(); ++i) if (FilletableEdge(TopoDS::Edge(map(i)), edgeFaces)) add(i); }
                else for (int i : edges) { if (i < 1 || i > map.Extent()) throw std::runtime_error("edge " + std::to_string(i) + " does not exist"); add(i); }
                if (!added) throw std::runtime_error("no edges to chamfer");
                mk.Build();
                if (!mk.IsDone()) throw std::runtime_error("the distance may be too large for the selected edges");
                return mk.Shape();
            });
        }
        Shape Transform(const Shape& shape, const glm::dmat4& matrix, std::string* error) {
            if (!shape) { SetError(error, "no shape"); return {}; }
            return Guard(error, "transform", [&] { return Moved(S(shape), Trsf(matrix)); });
        }
        Shape LinearPattern(const Shape& shape, const glm::dvec3& step, int count, std::string* error) {
            if (!shape || count < 1) { SetError(error, "nothing to pattern"); return {}; }
            return Guard(error, "linear pattern", [&] {
                std::vector<TopoDS_Shape> copies;
                for (int i = 0; i < count; ++i) { gp_Trsf t; t.SetTranslation(gp_Vec(step.x * i, step.y * i, step.z * i)); copies.push_back(i ? Moved(S(shape), t) : S(shape)); }
                return FuseAll(copies);
            });
        }
        Shape CircularPattern(const Shape& shape, const glm::dvec3& origin, const glm::dvec3& axis, int count, double totalDegrees, std::string* error) {
            if (!shape || count < 1 || !(glm::length(axis) > 1e-12)) { SetError(error, "nothing to pattern, or no axis"); return {}; }
            return Guard(error, "circular pattern", [&] {
                const bool full = std::fabs(std::fabs(totalDegrees) - 360.0) < 1e-9;
                const double step = (full ? totalDegrees / count : (count > 1 ? totalDegrees / (count - 1) : 0.0)) * 3.14159265358979323846 / 180.0;
                std::vector<TopoDS_Shape> copies;
                for (int i = 0; i < count; ++i) { gp_Trsf t; t.SetRotation(gp_Ax1(P(origin), D(axis)), step * i); copies.push_back(i ? Moved(S(shape), t) : S(shape)); }
                return FuseAll(copies);
            });
        }
        Shape Mirror(const Shape& shape, const glm::dvec3& origin, const glm::dvec3& normal, bool keepOriginal, std::string* error) {
            if (!shape || !(glm::length(normal) > 1e-12)) { SetError(error, "nothing to mirror, or no plane"); return {}; }
            return Guard(error, "mirror", [&] {
                gp_Trsf t;
                t.SetMirror(gp_Ax2(P(origin), D(normal)));
                const TopoDS_Shape m = Moved(S(shape), t);
                return keepOriginal ? FuseAll({ S(shape), m }) : m;
            });
        }
        Shape Compound(const std::vector<Shape>& shapes) {
            BRep_Builder b;
            TopoDS_Compound c;
            b.MakeCompound(c);
            int n = 0;
            for (const Shape& s : shapes) if (s) { b.Add(c, S(s)); ++n; }
            return n ? Wrap(c) : Shape();
        }

        Properties Measure(const Shape& shape) {
            Properties p;
            if (!shape) return p;
            try {
                const TopoDS_Shape& s = S(shape);
                GProp_GProps vol, surf;
                BRepGProp::VolumeProperties(s, vol);
                BRepGProp::SurfaceProperties(s, surf);
                p.volume = vol.Mass();
                p.area = surf.Mass();
                p.centroid = std::fabs(p.volume) > 0 ? G(vol.CentreOfMass()) : G(surf.CentreOfMass());
                Bnd_Box box;
                BRepBndLib::AddOptimal(s, box, false, false);
                if (!box.IsVoid()) {
                    double x0, y0, z0, x1, y1, z1;
                    box.Get(x0, y0, z0, x1, y1, z1);
                    p.boundsMin = { x0, y0, z0 };
                    p.boundsMax = { x1, y1, z1 };
                }
                TopTools_IndexedMapOfShape m;
                TopExp::MapShapes(s, TopAbs_SOLID, m); p.solids = m.Extent(); m.Clear();
                TopExp::MapShapes(s, TopAbs_FACE, m); p.faces = m.Extent(); m.Clear();
                TopExp::MapShapes(s, TopAbs_EDGE, m); p.edges = m.Extent(); m.Clear();
                TopExp::MapShapes(s, TopAbs_VERTEX, m); p.vertices = m.Extent();
                p.valid = Valid(s);
            }
            catch (...) { p.valid = false; }
            return p;
        }
        std::vector<FaceInfo> FacesOf(const Shape& shape) {
            std::vector<FaceInfo> out;
            if (!shape) return out;
            TopTools_IndexedMapOfShape map;
            TopExp::MapShapes(S(shape), TopAbs_FACE, map);
            for (int i = 1; i <= map.Extent(); ++i) {
                FaceInfo info;
                info.index = i;
                try {
                    const TopoDS_Face face = TopoDS::Face(map(i));
                    BRepAdaptor_Surface a(face);
                    switch (a.GetType()) {
                        case GeomAbs_Plane: info.surface = SurfaceType::Plane; break;
                        case GeomAbs_Cylinder: info.surface = SurfaceType::Cylinder; info.radius = a.Cylinder().Radius(); break;
                        case GeomAbs_Cone: info.surface = SurfaceType::Cone; info.radius = a.Cone().RefRadius(); break;
                        case GeomAbs_Sphere: info.surface = SurfaceType::Sphere; info.radius = a.Sphere().Radius(); break;
                        case GeomAbs_Torus: info.surface = SurfaceType::Torus; info.radius = a.Torus().MajorRadius(); break;
                        case GeomAbs_BSplineSurface: case GeomAbs_BezierSurface: info.surface = SurfaceType::BSpline; break;
                        default: info.surface = SurfaceType::Other; break;
                    }
                    GProp_GProps g;
                    BRepGProp::SurfaceProperties(face, g);
                    info.area = g.Mass();
                    info.centroid = G(g.CentreOfMass());
                    double u0, u1, v0, v1;
                    BRepTools::UVBounds(face, u0, u1, v0, v1);
                    BRepGProp_Face pf(face);
                    gp_Pnt pt;
                    gp_Vec n;
                    pf.Normal(0.5 * (u0 + u1), 0.5 * (v0 + v1), pt, n);   // oriented with the face
                    if (n.Magnitude() > 1e-12) info.normal = G(n.Normalized());
                }
                catch (...) {}
                out.push_back(info);
            }
            return out;
        }
        std::vector<EdgeInfo> EdgesOf(const Shape& shape) {
            std::vector<EdgeInfo> out;
            if (!shape) return out;
            TopTools_IndexedMapOfShape map;
            TopExp::MapShapes(S(shape), TopAbs_EDGE, map);
            for (int i = 1; i <= map.Extent(); ++i) {
                EdgeInfo info;
                info.index = i;
                try {
                    const TopoDS_Edge edge = TopoDS::Edge(map(i));
                    if (!BRep_Tool::Degenerated(edge)) {
                        BRepAdaptor_Curve c(edge);
                        switch (c.GetType()) {
                            case GeomAbs_Line: info.curve = CurveType::Line; break;
                            case GeomAbs_Circle: info.curve = CurveType::Circle; info.radius = c.Circle().Radius(); break;
                            case GeomAbs_Ellipse: info.curve = CurveType::Ellipse; break;
                            case GeomAbs_BSplineCurve: case GeomAbs_BezierCurve: info.curve = CurveType::BSpline; break;
                            default: info.curve = CurveType::Other; break;
                        }
                        const double t0 = c.FirstParameter(), t1 = c.LastParameter();
                        info.length = GCPnts_AbscissaPoint::Length(c);
                        gp_Pnt mid;
                        gp_Vec d;
                        c.D1(0.5 * (t0 + t1), mid, d);
                        info.midpoint = G(mid);
                        if (d.Magnitude() > 1e-12) info.tangent = G(d.Normalized());
                        info.start = G(c.Value(t0));
                        info.end = G(c.Value(t1));
                    }
                }
                catch (...) {}
                out.push_back(info);
            }
            return out;
        }
        Shape FaceShape(const Shape& shape, int index) {
            if (!shape) return {};
            TopTools_IndexedMapOfShape map;
            TopExp::MapShapes(S(shape), TopAbs_FACE, map);
            return index >= 1 && index <= map.Extent() ? Wrap(map(index)) : Shape();
        }
        Shape EdgeShape(const Shape& shape, int index) {
            if (!shape) return {};
            TopTools_IndexedMapOfShape map;
            TopExp::MapShapes(S(shape), TopAbs_EDGE, map);
            return index >= 1 && index <= map.Extent() ? Wrap(map(index)) : Shape();
        }
        bool Distance(const Shape& a, const Shape& b, double& distance, glm::dvec3* pa, glm::dvec3* pb) {
            if (!a || !b) return false;
            try {
                BRepExtrema_DistShapeShape d(S(a), S(b));
                if (!d.IsDone() || d.NbSolution() < 1) return false;
                distance = d.Value();
                if (pa) *pa = G(d.PointOnShape1(1));
                if (pb) *pb = G(d.PointOnShape2(1));
                return true;
            }
            catch (...) { return false; }
        }
        bool DistanceToPoint(const Shape& shape, const glm::dvec3& point, double& distance, glm::dvec3* closest) {
            if (!shape) return false;
            try {
                const TopoDS_Shape v = BRepBuilderAPI_MakeVertex(P(point)).Shape();
                BRepExtrema_DistShapeShape d(S(shape), v);
                if (!d.IsDone() || d.NbSolution() < 1) return false;
                distance = d.Value();
                if (closest) *closest = G(d.PointOnShape1(1));
                return true;
            }
            catch (...) { return false; }
        }
        Tessellation Tessellate(const Shape& shape, double linear, double angular) {
            Tessellation t;
            if (!shape) return t;
            try {
                const TopoDS_Shape& s = S(shape);
                BRepMesh_IncrementalMesh mesher(s, std::max(linear, 1e-6), false, std::max(angular, 0.05), true);
                TopTools_IndexedMapOfShape faces;
                TopExp::MapShapes(s, TopAbs_FACE, faces);
                for (int fi = 1; fi <= faces.Extent(); ++fi) {
                    const TopoDS_Face face = TopoDS::Face(faces(fi));
                    TopLoc_Location loc;
                    Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
                    if (tri.IsNull() || tri->NbTriangles() == 0) continue;
                    BRepLib_ToolTriangulatedShape::ComputeNormals(face, tri);
                    const gp_Trsf tr = loc.Transformation();
                    const bool reversed = face.Orientation() == TopAbs_REVERSED;
                    const uint32_t base = uint32_t(t.positions.size());
                    for (int n = 1; n <= tri->NbNodes(); ++n) {
                        const gp_Pnt p = tri->Node(n).Transformed(tr);
                        gp_Dir nd = tri->HasNormals() ? tri->Normal(n) : gp_Dir(0, 0, 1);
                        nd.Transform(tr);
                        if (reversed) nd.Reverse();
                        t.positions.emplace_back(float(p.X()), float(p.Y()), float(p.Z()));
                        t.normals.emplace_back(float(nd.X()), float(nd.Y()), float(nd.Z()));
                    }
                    for (int k = 1; k <= tri->NbTriangles(); ++k) {
                        int a, b, c;
                        tri->Triangle(k).Get(a, b, c);
                        if (reversed) std::swap(b, c);
                        t.indices.push_back(base + uint32_t(a - 1));
                        t.indices.push_back(base + uint32_t(b - 1));
                        t.indices.push_back(base + uint32_t(c - 1));
                        t.faceOf.push_back(fi);
                    }
                }
                TopTools_IndexedMapOfShape edges;
                TopExp::MapShapes(s, TopAbs_EDGE, edges);
                for (int ei = 1; ei <= edges.Extent(); ++ei) {
                    const TopoDS_Edge edge = TopoDS::Edge(edges(ei));
                    if (BRep_Tool::Degenerated(edge)) continue;
                    try {
                        BRepAdaptor_Curve c(edge);
                        GCPnts_TangentialDeflection d(c, std::max(angular, 0.05), std::max(linear, 1e-6));
                        Tessellation::Polyline line;
                        line.edge = ei;
                        for (int k = 1; k <= d.NbPoints(); ++k) { const gp_Pnt p = d.Value(k); line.points.emplace_back(float(p.X()), float(p.Y()), float(p.Z())); }
                        if (line.points.size() >= 2) t.edges.push_back(std::move(line));
                    }
                    catch (...) {}
                }
            }
            catch (...) { t = {}; }
            return t;
        }

        Shape Import(const std::string& path, FileFormat format, std::string* error) {
            QuietKernel();
            if (!std::ifstream(path, std::ios::binary)) { SetError(error, "cannot open '" + path + "'"); return {}; }
            try {
                TopoDS_Shape s;
                if (format == FileFormat::Step) {
                    STEPControl_Controller::Init();
                    Interface_Static::SetCVal("xstep.cascade.unit", "MM");
                    STEPControl_Reader r;
                    if (r.ReadFile(path.c_str()) != IFSelect_RetDone) { SetError(error, "not a readable STEP file"); return {}; }
                    if (r.TransferRoots() == 0) { SetError(error, "the STEP file contains no shape PHISIKS can read"); return {}; }
                    s = r.OneShape();
                }
                else if (format == FileFormat::Iges) {
                    IGESControl_Controller::Init();
                    Interface_Static::SetCVal("xstep.cascade.unit", "MM");
                    IGESControl_Reader r;
                    if (r.ReadFile(path.c_str()) != IFSelect_RetDone) { SetError(error, "not a readable IGES file"); return {}; }
                    if (r.TransferRoots() == 0) { SetError(error, "the IGES file contains no shape PHISIKS can read"); return {}; }
                    s = r.OneShape();
                }
                else if (format == FileFormat::Brep) {
                    BRep_Builder b;
                    if (!BRepTools::Read(s, path.c_str(), b)) { SetError(error, "not a readable BREP file"); return {}; }
                }
                else {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)   // StlAPI::Read: one face per triangle (a faceted shell, documented)
#endif
                    const bool read = StlAPI::Read(s, path.c_str());
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
                    if (!read) { SetError(error, "not a readable STL file"); return {}; }
                }
                if (s.IsNull()) { SetError(error, "the file contains no geometry"); return {}; }
                return Wrap(s);
            }
            catch (const Standard_Failure& e) { SetError(error, FailureText(e)); }
            catch (const std::exception& e) { SetError(error, e.what()); }
            return {};
        }
        bool Export(const Shape& shape, const std::string& path, FileFormat format, std::string* error, double stlDeflection) {
            QuietKernel();
            if (!shape) { SetError(error, "nothing to export"); return false; }
            try {
                bool ok = false;
                if (format == FileFormat::Step) {
                    STEPControl_Controller::Init();
                    Interface_Static::SetCVal("write.step.unit", "MM");
                    STEPControl_Writer w;
                    if (w.Transfer(S(shape), STEPControl_AsIs) != IFSelect_RetDone) { SetError(error, "STEP translation failed"); return false; }
                    ok = w.Write(path.c_str()) == IFSelect_RetDone;
                }
                else if (format == FileFormat::Iges) {
                    IGESControl_Controller::Init();
                    IGESControl_Writer w("MM", 1);   // 1: B-rep faces (exact solids)
                    if (!w.AddShape(S(shape))) { SetError(error, "IGES translation failed"); return false; }
                    w.ComputeModel();
                    ok = w.Write(path.c_str());
                }
                else if (format == FileFormat::Brep) ok = BRepTools::Write(S(shape), path.c_str());
                else {
                    BRepMesh_IncrementalMesh mesher(S(shape), std::max(stlDeflection, 1e-6), false, 0.35, true);
                    StlAPI_Writer w;
                    w.ASCIIMode() = false;
                    ok = w.Write(S(shape), path.c_str());
                }
                if (!ok) SetError(error, "cannot write '" + path + "'");
                return ok;
            }
            catch (const Standard_Failure& e) { SetError(error, FailureText(e)); }
            catch (const std::exception& e) { SetError(error, e.what()); }
            return false;
        }
        std::string ToBrep(const Shape& shape) {
            if (!shape) return {};
            try { std::ostringstream o; BRepTools::Write(S(shape), o); return o.str(); }
            catch (...) { return {}; }
        }
        Shape FromBrep(const std::string& text, std::string* error) {
            // Only text that is a BREP reaches the OCCT reader (which prints diagnostics for others).
            if (text.substr(0, 128).find("CASCADE Topology") == std::string::npos) { SetError(error, "the stored geometry is not BREP text"); return {}; }
            QuietKernel();
            try {
                std::istringstream in(text);
                TopoDS_Shape s;
                BRep_Builder b;
                BRepTools::Read(s, in, b);
                if (s.IsNull()) { SetError(error, "the stored geometry is empty or damaged"); return {}; }
                return Wrap(s);
            }
            catch (const Standard_Failure& e) { SetError(error, FailureText(e)); }
            catch (const std::exception& e) { SetError(error, e.what()); }
            return {};
        }
    }

#else   // ======================================================================== no kernel
    struct ShapeData {};
    namespace {
        Shape None(std::string* error) { if (error) *error = "no geometry kernel (PHISIKS was built without Open CASCADE)"; return {}; }
    }
    namespace Kernel {
        bool Available() { return false; }
        std::string Name() { return "none"; }
        Shape Face(const Profile&, const Plane&, std::string* e) { return None(e); }
        Shape Faces(const std::vector<Profile>&, const Plane&, std::string* e) { return None(e); }
        Shape Extrude(const Shape&, const glm::dvec3&, bool, std::string* e) { return None(e); }
        Shape Revolve(const Shape&, const glm::dvec3&, const glm::dvec3&, double, std::string* e) { return None(e); }
        Shape Box(const glm::dvec3&, const glm::dvec3&, std::string* e) { return None(e); }
        Shape Cylinder(const glm::dvec3&, const glm::dvec3&, double, double, std::string* e) { return None(e); }
        Shape Sphere(const glm::dvec3&, double, std::string* e) { return None(e); }
        Shape Boolean(const Shape&, const Shape&, BooleanOp, std::string* e) { return None(e); }
        Shape Fillet(const Shape&, const std::vector<int>&, double, std::string* e) { return None(e); }
        Shape Chamfer(const Shape&, const std::vector<int>&, double, std::string* e) { return None(e); }
        Shape Transform(const Shape&, const glm::dmat4&, std::string* e) { return None(e); }
        Shape LinearPattern(const Shape&, const glm::dvec3&, int, std::string* e) { return None(e); }
        Shape CircularPattern(const Shape&, const glm::dvec3&, const glm::dvec3&, int, double, std::string* e) { return None(e); }
        Shape Mirror(const Shape&, const glm::dvec3&, const glm::dvec3&, bool, std::string* e) { return None(e); }
        Shape Compound(const std::vector<Shape>&) { return {}; }
        Properties Measure(const Shape&) { return {}; }
        std::vector<FaceInfo> FacesOf(const Shape&) { return {}; }
        std::vector<EdgeInfo> EdgesOf(const Shape&) { return {}; }
        Shape FaceShape(const Shape&, int) { return {}; }
        Shape EdgeShape(const Shape&, int) { return {}; }
        bool Distance(const Shape&, const Shape&, double&, glm::dvec3*, glm::dvec3*) { return false; }
        bool DistanceToPoint(const Shape&, const glm::dvec3&, double&, glm::dvec3*) { return false; }
        Tessellation Tessellate(const Shape&, double, double) { return {}; }
        Shape Import(const std::string&, FileFormat, std::string* e) { return None(e); }
        bool Export(const Shape&, const std::string&, FileFormat, std::string* e, double) { None(e); return false; }
        std::string ToBrep(const Shape&) { return {}; }
        Shape FromBrep(const std::string&, std::string* e) { return None(e); }
    }
#endif
}
