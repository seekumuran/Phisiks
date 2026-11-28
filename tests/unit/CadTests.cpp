// CAD (PhisiksCad): sketch constraints and solver, degrees of freedom, conflicts, profiles,
// expressions and parameters, the parametric document (features, regeneration, dependencies,
// persistence) and the Open CASCADE kernel (solids, booleans, fillets, patterns, measurement,
// STEP / IGES / STL exchange, units). Kernel tests are skipped when PHISIKS is built without one.
#include "TestFramework.h"

#include "Phisiks/Cad/Document.h"
#include "Phisiks/Cad/Expression.h"
#include "Phisiks/Cad/Kernel.h"
#include "Phisiks/Cad/Sketch.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace Phisiks::Cad;
namespace fs = std::filesystem;

namespace {
    constexpr double PI = 3.14159265358979323846;
    struct TempDir {
        fs::path path;
        TempDir() {
            path = fs::temp_directory_path() / ("phisiks_cad_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            fs::create_directories(path);
        }
        ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
    };
    // a w x h rectangle with its first corner fixed at the origin and two driving dimensions
    struct Rect { Sketch s; std::vector<Id> lines; Id width = 0, height = 0; };
    Rect DimensionedRect(double w, double h) {
        Rect r;
        r.lines = r.s.AddRectangle(0, 0, w * 0.9, h * 1.1);   // start off-size: the dimensions must move it
        r.s.AddConstraint(ConstraintType::Fix, r.s.Find(r.lines[0])->a);
        r.width = r.s.AddConstraint(ConstraintType::Length, r.lines[0], 0, 0, w);
        r.height = r.s.AddConstraint(ConstraintType::Length, r.lines[1], 0, 0, h);
        return r;
    }
    Feature SketchFeature(const Sketch& s, const char* plane = "XY") { Feature f; f.type = FeatureType::Sketch; f.sketch = s; f.plane = plane; return f; }
    Feature ExtrudeFeature(Id sketch, const std::string& distance, FeatureOp op = FeatureOp::NewBody) {
        Feature f; f.type = FeatureType::Extrude; f.profileSketch = sketch; f.values["distance"] = distance; f.op = op; return f;
    }
    double VolumeOf(const Document& d) { return Kernel::Measure(d.Result()).volume; }
}

// ============================================================================ sketch solver
TEST_CASE("CAD sketch: rectangle dimensions drive the geometry, fully constrained") {
    Rect r = DimensionedRect(40, 20);
    const SolveResult res = r.s.Solve();
    CHECK(res.status == SolveStatus::Solved);
    CHECK(res.degreesOfFreedom == 0);
    CHECK(res.FullyConstrained());
    CHECK(res.redundant.empty());
    const Geometry* top = r.s.Find(r.lines[2]);
    const glm::dvec2 c = r.s.PointPosition(top->a);   // the far corner
    CHECK_NEAR(c.x, 40.0, 1e-7);
    CHECK_NEAR(c.y, 20.0, 1e-7);
    // changing a dimension updates the dependent geometry
    CHECK(r.s.SetConstraintValue(r.width, 55.0));
    CHECK(r.s.Solve().status == SolveStatus::Solved);
    CHECK_NEAR(r.s.PointPosition(r.s.Find(r.lines[2])->a).x, 55.0, 1e-7);
    CHECK_NEAR(r.s.PointPosition(r.s.Find(r.lines[2])->a).y, 20.0, 1e-7);
}

TEST_CASE("CAD sketch: degrees of freedom of under-constrained sketches") {
    Sketch s;
    std::vector<Id> l = s.AddRectangle(0, 0, 10, 5);
    SolveResult res = s.Solve();
    CHECK(res.status == SolveStatus::Solved);
    CHECK(res.degreesOfFreedom == 4);    // position (2) + width + height
    s.AddConstraint(ConstraintType::Fix, s.Find(l[0])->a);
    CHECK(s.Solve().degreesOfFreedom == 2);
    s.AddConstraint(ConstraintType::Equal, l[0], l[1]);   // a square
    CHECK(s.Solve().degreesOfFreedom == 1);
    s.AddDimension(ConstraintType::Length, l[0]);
    res = s.Solve();
    CHECK(res.degreesOfFreedom == 0);
    CHECK_NEAR(s.Measure(ConstraintType::Length, l[1]), s.Measure(ConstraintType::Length, l[0]), 1e-7);
    // a lone point has 2, a free circle 3
    Sketch t;
    t.AddPoint(1, 2);
    t.AddCircleAt(0, 0, 3);
    CHECK(t.Solve().degreesOfFreedom == 5);
}

TEST_CASE("CAD sketch: conflicting constraints fail and leave the sketch unchanged") {
    Sketch s;
    const Id line = s.AddLine(0, 0, 10, 0);
    s.AddConstraint(ConstraintType::Fix, s.Find(line)->a);
    s.AddConstraint(ConstraintType::Length, line, 0, 0, 10);
    CHECK(s.Solve().status == SolveStatus::Solved);
    const Id conflict = s.AddConstraint(ConstraintType::Length, line, 0, 0, 20);
    const glm::dvec2 before = s.PointPosition(s.Find(line)->b);
    const SolveResult res = s.Solve();
    CHECK(res.status == SolveStatus::Failed);
    CHECK(!res.message.empty());
    CHECK(std::find(res.redundant.begin(), res.redundant.end(), conflict) != res.redundant.end());
    CHECK(s.PointPosition(s.Find(line)->b) == before);
    CHECK(s.RemoveConstraint(conflict));
    CHECK(s.Solve().status == SolveStatus::Solved);
    // redundant but consistent: solved, and reported
    s.AddConstraint(ConstraintType::Horizontal, line);
    const Id again = s.AddConstraint(ConstraintType::Horizontal, line);
    const SolveResult r2 = s.Solve();
    CHECK(r2.status == SolveStatus::Solved);
    CHECK(std::find(r2.redundant.begin(), r2.redundant.end(), again) != r2.redundant.end());
}

TEST_CASE("CAD sketch: geometric constraints") {
    Sketch s;
    // perpendicular + parallel + angle
    const Id base = s.AddLine(0, 0, 10, 0);
    s.AddConstraint(ConstraintType::Fix, s.Find(base)->a);
    s.AddConstraint(ConstraintType::Fix, s.Find(base)->b);
    const Id up = s.AddLine(0, 0.2, 1, 8);
    s.AddConstraint(ConstraintType::Coincident, s.Find(up)->a, s.Find(base)->a);
    s.AddConstraint(ConstraintType::Perpendicular, base, up);
    const Id other = s.AddLine(5, 1, 6, 7);
    s.AddConstraint(ConstraintType::Parallel, up, other);
    const Id slanted = s.AddLine(10, 0, 14, 2);
    s.AddConstraint(ConstraintType::Coincident, s.Find(slanted)->a, s.Find(base)->b);
    s.AddConstraint(ConstraintType::Angle, base, slanted, 0, 30.0);
    // tangent circle to the base line, concentric arc, point on circle, midpoint, equal, symmetric
    const Id circle = s.AddCircleAt(3, 4, 2);
    s.AddConstraint(ConstraintType::Tangent, base, circle);
    s.AddConstraint(ConstraintType::Radius, circle, 0, 0, 3.0);
    const Id arc = s.AddArc(3, 4, 5, 0, 90);
    s.AddConstraint(ConstraintType::Concentric, arc, circle);
    const Id onCircle = s.AddPoint(6, 6);
    s.AddConstraint(ConstraintType::PointOnCircle, onCircle, circle);
    const Id mid = s.AddPoint(4, 1);
    s.AddConstraint(ConstraintType::Midpoint, mid, base);
    const Id a = s.AddPoint(2, 3), b = s.AddPoint(7, 2);
    s.AddConstraint(ConstraintType::Symmetric, a, b, up);
    const SolveResult res = s.Solve();
    CHECK(res.status == SolveStatus::Solved);
    CHECK_NEAR(s.Measure(ConstraintType::Angle, base, up), 90.0, 1e-6);
    CHECK_NEAR(std::fabs(s.Measure(ConstraintType::Angle, up, other)) < 90 ? s.Measure(ConstraintType::Angle, up, other) : 0.0, 0.0, 1e-6);
    CHECK_NEAR(s.Measure(ConstraintType::Angle, base, slanted), 30.0, 1e-6);
    CHECK_NEAR(s.RadiusOf(circle), 3.0, 1e-7);
    CHECK_NEAR(s.PointPosition(s.Find(circle)->a).y, 3.0, 1e-7);      // tangent to y = 0 with r = 3
    CHECK(glm::length(s.PointPosition(s.Find(arc)->a) - s.PointPosition(s.Find(circle)->a)) < 1e-7);
    CHECK_NEAR(glm::length(s.PointPosition(onCircle) - s.PointPosition(s.Find(circle)->a)), 3.0, 1e-7);
    CHECK_NEAR(s.PointPosition(mid).x, 5.0, 1e-7);
    CHECK_NEAR(s.PointPosition(mid).y, 0.0, 1e-7);
    CHECK_NEAR(s.PointPosition(a).y, s.PointPosition(b).y, 1e-7);      // mirrored about the vertical line x = 0
    CHECK_NEAR(s.PointPosition(a).x, -s.PointPosition(b).x, 1e-7);
    // arcs keep both ends on one circle
    CHECK_NEAR(glm::length(s.PointPosition(s.Find(arc)->b) - s.PointPosition(s.Find(arc)->a)),
               glm::length(s.PointPosition(s.Find(arc)->c) - s.PointPosition(s.Find(arc)->a)), 1e-7);
    // invalid constraints are refused
    CHECK(s.AddConstraint(ConstraintType::Parallel, base, circle) == 0);
    CHECK(s.AddConstraint(ConstraintType::Length, circle, 0, 0, 3) == 0);
    CHECK(s.AddConstraint(ConstraintType::Length, base, 0, 0, -1) == 0);
    CHECK(s.AddConstraint(ConstraintType::Coincident, 999, 1000) == 0);
}

TEST_CASE("CAD sketch: profiles, holes and open chains") {
    Sketch s;
    s.AddRectangle(0, 0, 40, 20);
    s.AddCircleAt(20, 10, 5);
    s.AddLine(-10, -10, -5, -5, true);    // construction: ignored
    ProfileResult pr = s.Profiles();
    CHECK(pr.ok());
    CHECK(pr.profiles.size() == 1);
    CHECK(pr.profiles.size() == 1 && pr.profiles[0].holes.size() == 1);
    CHECK_NEAR(std::fabs(pr.profiles[0].outer.signedArea), 800.0, 1e-6);
    CHECK_NEAR(std::fabs(pr.profiles[0].holes[0].signedArea), PI * 25.0, 1e-6);
    // slot: two lines + two half circles
    Sketch slot;
    const Id c1 = slot.AddPoint(0, 0), c2 = slot.AddPoint(10, 0);
    const Id a = slot.AddPoint(0, -2), b = slot.AddPoint(10, -2), c = slot.AddPoint(10, 2), d = slot.AddPoint(0, 2);
    slot.AddLine(a, b);
    slot.AddArc(c2, b, c);
    slot.AddLine(c, d);
    slot.AddArc(c1, d, a);
    pr = slot.Profiles();
    CHECK(pr.ok());
    CHECK(pr.profiles.size() == 1);
    CHECK_NEAR(std::fabs(pr.profiles[0].outer.signedArea), 40.0 + PI * 4.0, 1e-6);
    // open chain
    Sketch open;
    open.AddLine(0, 0, 10, 0);
    open.AddLine(10, 0, 10, 10);
    pr = open.Profiles();
    CHECK(!pr.ok());
    CHECK(pr.error.find("open") != std::string::npos);
    Sketch empty;
    CHECK(!empty.Profiles().ok());
}

TEST_CASE("CAD sketch: JSON round trip and corrupt input") {
    Rect r = DimensionedRect(30, 10);
    r.s.SetConstraintExpression(r.width, "w");
    r.s.Solve();
    Sketch back;
    std::string error;
    CHECK(Sketch::FromJson(r.s.ToJson(), back, &error));
    CHECK(back.Geometries().size() == r.s.Geometries().size());
    CHECK(back.Constraints().size() == r.s.Constraints().size());
    CHECK(back.FindConstraint(r.width) && back.FindConstraint(r.width)->expression == "w");
    CHECK(back.Solve().degreesOfFreedom == 0);
    CHECK(back.NextId() == r.s.NextId());
    Sketch bad;
    CHECK(!Sketch::FromJson(R"({"geometry":[{"id":1,"type":"line","a":5,"b":6}],"constraints":[]})", bad, &error));
    CHECK(!Sketch::FromJson(R"({"geometry":[{"id":1,"type":"blob"}],"constraints":[]})", bad, &error));
    CHECK(!Sketch::FromJson("not json", bad, &error));
}

// ============================================================================ expressions, parameters
TEST_CASE("CAD expressions and parameters") {
    auto lookup = [](const std::string& n, double& v) { if (n == "w") { v = 4; return true; } return false; };
    double v = 0;
    CHECK(Evaluate("2 * (w + 3)", lookup, v)); CHECK_NEAR(v, 14.0, 1e-12);
    CHECK(Evaluate("-w ^ 2 + max(1, 2) + sqrt(16) + cos(60)", lookup, v)); CHECK_NEAR(v, -16 + 2 + 4 + 0.5, 1e-12);
    CHECK(Evaluate("2 * pi", lookup, v)); CHECK_NEAR(v, 2 * PI, 1e-12);
    std::string error;
    CHECK(!Evaluate("w + depth", lookup, v, &error)); CHECK(error.find("depth") != std::string::npos);
    CHECK(!Evaluate("1 / (w - 4)", lookup, v, &error));
    CHECK(!Evaluate("(1 + 2", lookup, v, &error));
    CHECK(!Evaluate("3 4", lookup, v, &error));
    CHECK(ExpressionNames("a * b + sin(c) + pi").size() == 3);
    CHECK(IsValidParameterName("width_2"));
    CHECK(!IsValidParameterName("2w"));
    CHECK(!IsValidParameterName("sin"));

    Document d;
    CHECK(d.SetParameter("width", "40"));
    CHECK(d.SetParameter("half", "width / 2"));
    CHECK(d.ParameterValue("half", v)); CHECK_NEAR(v, 20.0, 1e-12);
    CHECK(d.SetParameter("width", "60"));
    CHECK(d.ParameterValue("half", v)); CHECK_NEAR(v, 30.0, 1e-12);
    CHECK(!d.SetParameter("width", "half * 2", &error));    // cycle
    CHECK(error.find("circular") != std::string::npos);
    CHECK(d.ParameterValue("width", v)); CHECK_NEAR(v, 60.0, 1e-12);   // unchanged
    CHECK(!d.SetParameter("bad name", "1"));
    CHECK(!d.RemoveParameter("width", &error));              // used by 'half'
    CHECK(d.RemoveParameter("half"));
    CHECK(d.RemoveParameter("width"));
}

// ============================================================================ document (no kernel needed)
TEST_CASE("CAD document: dependencies, ordering, removal, persistence") {
    Document d;
    Rect r = DimensionedRect(40, 20);
    const Id sk = d.AddFeature(SketchFeature(r.s));
    const Id ex = d.AddFeature(ExtrudeFeature(sk, "10"));
    Feature fillet; fillet.type = FeatureType::Fillet; fillet.values["radius"] = "2";
    const Id fi = d.AddFeature(fillet);
    CHECK(sk && ex && fi);
    CHECK(d.Dependencies(*d.FindFeature(ex)) == std::vector<Id>{ sk });
    CHECK(d.Dependencies(*d.FindFeature(fi)) == std::vector<Id>{ ex });
    CHECK(d.Dependents(sk) == std::vector<Id>{ ex });
    std::string error;
    CHECK(!d.RemoveFeature(sk, &error));                      // the extrude uses it
    CHECK(error.find("Extrude") != std::string::npos);
    CHECK(!d.MoveFeature(ex, 0, &error));                     // before its sketch
    CHECK(d.IndexOf(ex) == 1);
    CHECK(d.InsertFeature(ExtrudeFeature(ex + 100, "5"), 0) == 0);   // unknown sketch
    CHECK(d.FindFeature(ex)->name == "Extrude 1");
    // persistence
    d.unit = Unit::Centimetre;
    CHECK(d.SetParameter("depth", "12"));
    Feature e = *d.FindFeature(ex);
    e.values["distance"] = "depth";
    CHECK(d.UpdateFeature(e));
    Document back;
    CHECK(Document::FromJson(d.ToJson(), back, &error));
    CHECK(back.unit == Unit::Centimetre);
    CHECK(back.Features().size() == 3);
    CHECK(back.FindFeature(ex)->values.at("distance") == "depth");
    CHECK(back.FindFeature(sk)->sketch.Constraints().size() == r.s.Constraints().size());
    CHECK(back.Parameters().size() == 1);
    CHECK(back.ToJson() == d.ToJson());
    // rejected documents
    Document bad;
    CHECK(!Document::FromJson(R"({"format":"phisiks-cad","version":99,"features":[]})", bad, &error));
    CHECK(error.find("version") != std::string::npos);
    CHECK(!Document::FromJson(R"({"format":"something-else","version":1})", bad, &error));
    CHECK(!Document::FromJson(R"({"format":"phisiks-cad","version":1,"features":[{"id":2,"type":"extrude","sketch":7}]})", bad, &error));
    CHECK(!Document::FromJson(R"({"format":"phisiks-cad","version":1,"features":[{"id":1,"type":"warp"}]})", bad, &error));
}

// ============================================================================ kernel + regeneration
TEST_CASE("CAD kernel: extrude, parametric update, cut, booleans") {
    if (!Kernel::Available()) { std::printf("    (skipped: no geometry kernel)\n"); return; }
    Document d;
    CHECK(d.SetParameter("width", "40"));
    Rect r = DimensionedRect(40, 20);
    r.s.SetConstraintExpression(r.width, "width");
    const Id sk = d.AddFeature(SketchFeature(r.s));
    const Id ex = d.AddFeature(ExtrudeFeature(sk, "10"));
    RegenerateResult res = d.Regenerate();
    CHECK(res.ok);
    CHECK(d.Bodies().size() == 1);
    CHECK_NEAR(VolumeOf(d), 40 * 20 * 10, 1e-6);
    const Properties p = Kernel::Measure(d.Result());
    CHECK(p.valid);
    CHECK(p.faces == 6);
    CHECK(p.edges == 12);
    CHECK_NEAR(p.area, 2 * (40 * 20 + 40 * 10 + 20 * 10), 1e-6);
    CHECK_NEAR(p.boundsMax.x - p.boundsMin.x, 40, 1e-6);
    // the parameter drives the sketch dimension, the extrude follows
    CHECK(d.SetParameter("width", "60"));
    CHECK(d.Regenerate().ok);
    CHECK_NEAR(VolumeOf(d), 60 * 20 * 10, 1e-6);
    // a through hole: circle sketch on the top face plane, cut
    Sketch hole;
    hole.AddCircleAt(30, 10, 5);
    Feature hs = SketchFeature(hole);
    hs.values["offset"] = "10";
    const Id hsk = d.AddFeature(hs);
    Feature cut = ExtrudeFeature(hsk, "10", FeatureOp::Cut);
    cut.reverse = true;
    const Id cutId = d.AddFeature(cut);
    CHECK(d.Regenerate().ok);
    CHECK_NEAR(VolumeOf(d), 60 * 20 * 10 - PI * 25 * 10, 1e-3);
    CHECK(d.State(cutId)->built);
    CHECK(d.State(cutId)->properties.faces == 7);
    // booleans with a second body
    Feature cyl; cyl.type = FeatureType::Cylinder; cyl.position = { 0, 0, 0 }; cyl.direction = { 0, 0, 1 };
    cyl.values["radius"] = "10"; cyl.values["height"] = "10";
    const Id cy = d.AddFeature(cyl);
    CHECK(d.Regenerate().ok);
    CHECK(d.Bodies().size() == 2);
    Feature boolean; boolean.type = FeatureType::Boolean; boolean.body = ex; boolean.tool = cy; boolean.boolean = BooleanOp::Subtract;
    const Id bo = d.AddFeature(boolean);
    CHECK(d.Regenerate().ok);
    CHECK(d.Bodies().size() == 1);
    CHECK_NEAR(VolumeOf(d), 60 * 20 * 10 - PI * 25 * 10 - PI * 100 * 10 / 4, 1e-2);   // a quarter of the cylinder is inside the block
    Feature b2 = *d.FindFeature(bo); b2.boolean = BooleanOp::Intersect;
    CHECK(d.UpdateFeature(b2));
    CHECK(d.Regenerate().ok);
    CHECK_NEAR(VolumeOf(d), PI * 100 * 10 / 4, 1e-2);
    b2.boolean = BooleanOp::Union;
    CHECK(d.UpdateFeature(b2));
    CHECK(d.Regenerate().ok);
    CHECK_NEAR(VolumeOf(d), 60 * 20 * 10 - PI * 25 * 10 + PI * 100 * 10 * 0.75, 1e-2);
}

TEST_CASE("CAD kernel: revolve, fillet, chamfer, patterns, mirror") {
    if (!Kernel::Available()) { std::printf("    (skipped: no geometry kernel)\n"); return; }
    // revolve a 10..20 x 0..5 rectangle about the sketch y axis: a washer
    Document d;
    Sketch s;
    s.AddRectangle(10, 0, 20, 5);
    const Id sk = d.AddFeature(SketchFeature(s));
    Feature rev; rev.type = FeatureType::Revolve; rev.profileSketch = sk; rev.axis = "y"; rev.values["angle"] = "360";
    d.AddFeature(rev);
    CHECK(d.Regenerate().ok);
    CHECK_NEAR(VolumeOf(d), PI * (400 - 100) * 5, 1e-3);
    Feature half = d.Features()[1]; half.values["angle"] = "180";
    CHECK(d.UpdateFeature(half));
    CHECK(d.Regenerate().ok);
    CHECK_NEAR(VolumeOf(d), PI * (400 - 100) * 5 / 2, 1e-3);

    // fillet every edge of a box, then chamfer one selected edge of another
    Document f;
    Feature box; box.type = FeatureType::Box; box.values = { { "x", "20" }, { "y", "20" }, { "z", "20" } };
    const Id bx = f.AddFeature(box);
    Feature fil; fil.type = FeatureType::Fillet; fil.values["radius"] = "2";
    const Id fl = f.AddFeature(fil);
    CHECK(f.Regenerate().ok);
    const double filleted = VolumeOf(f);
    // exact rounded cube: core + 6 face slabs + 12 quarter cylinders + 8 sphere octants
    CHECK_NEAR(filleted, 16.0 * 16 * 16 + 6 * 16.0 * 16 * 2 + 12 * 16.0 * PI * 4 / 4 + 4.0 / 3.0 * PI * 8, 1e-3);
    CHECK(f.State(fl)->properties.faces == 26);    // 6 faces + 12 edge rounds + 8 corner patches
    // too large: a clear error, the body stays at the previous feature's result
    Feature big = *f.FindFeature(fl); big.values["radius"] = "15";
    CHECK(f.UpdateFeature(big));
    RegenerateResult r = f.Regenerate();
    CHECK(!r.ok);
    CHECK(!f.State(fl)->built);
    CHECK(!f.State(fl)->error.empty());
    CHECK_NEAR(VolumeOf(f), 8000.0, 1e-6);
    CHECK(f.RemoveFeature(fl));
    CHECK(f.Regenerate().ok);
    Shape boxShape = f.ShapeAfter(bx);
    int topFront = 0;
    for (const EdgeInfo& e : Kernel::EdgesOf(boxShape))
        if (std::fabs(e.midpoint.y - 0) < 1e-6 && std::fabs(e.midpoint.z - 20) < 1e-6) topFront = e.index;
    CHECK(topFront != 0);
    Feature ch; ch.type = FeatureType::Chamfer; ch.values["distance"] = "2"; ch.edges = { Document::MakeEdgeRef(boxShape, topFront) };
    const Id chId = f.AddFeature(ch);
    CHECK(f.Regenerate().ok);
    CHECK_NEAR(VolumeOf(f), 8000.0 - 0.5 * 2 * 2 * 20, 1e-6);
    // the edge reference follows the model when the box grows
    Feature bigger = *f.FindFeature(bx); bigger.values["x"] = "30";
    CHECK(f.UpdateFeature(bigger));
    CHECK(f.Regenerate().ok);
    CHECK(f.State(chId)->built);
    CHECK_NEAR(VolumeOf(f), 30 * 20 * 20 - 0.5 * 2 * 2 * 30, 1e-6);

    // feature pattern: a hole repeated along x
    Document p;
    Feature plate; plate.type = FeatureType::Box; plate.values = { { "x", "100" }, { "y", "20" }, { "z", "5" } };
    p.AddFeature(plate);
    Sketch hole; hole.AddCircleAt(10, 10, 3);
    const Id hsk = p.AddFeature(SketchFeature(hole));
    Feature hc = ExtrudeFeature(hsk, "5", FeatureOp::Cut);
    const Id holeId = p.AddFeature(hc);
    Feature lp; lp.type = FeatureType::LinearPattern; lp.source = holeId; lp.direction = { 1, 0, 0 }; lp.values = { { "count", "4" }, { "spacing", "25" } };
    const Id lpId = p.AddFeature(lp);
    CHECK(p.Regenerate().ok);
    CHECK_NEAR(VolumeOf(p), 100 * 20 * 5 - 4 * PI * 9 * 5, 1e-2);
    Feature lp2 = *p.FindFeature(lpId); lp2.values["count"] = "2.5";
    CHECK(p.UpdateFeature(lp2));
    CHECK(!p.Regenerate().ok);   // not a whole number
    // circular pattern of a body, mirror
    Document c;
    Feature pin; pin.type = FeatureType::Cylinder; pin.position = { 20, 0, 0 }; pin.direction = { 0, 0, 1 }; pin.values = { { "radius", "2" }, { "height", "5" } };
    c.AddFeature(pin);
    Feature cp; cp.type = FeatureType::CircularPattern; cp.origin = { 0, 0, 0 }; cp.direction = { 0, 0, 1 }; cp.values = { { "count", "6" }, { "angle", "360" } };
    c.AddFeature(cp);
    CHECK(c.Regenerate().ok);
    CHECK_NEAR(VolumeOf(c), 6 * PI * 4 * 5, 1e-3);
    CHECK(Kernel::Measure(c.Result()).solids == 6);
    Feature mi; mi.type = FeatureType::Mirror; mi.origin = { 0, 0, 0 }; mi.direction = { 0, 0, 1 }; mi.keepOriginal = true;
    c.AddFeature(mi);
    CHECK(c.Regenerate().ok);
    CHECK_NEAR(VolumeOf(c), 12 * PI * 4 * 5, 1e-3);
}

TEST_CASE("CAD kernel: failures are reported per feature, independent features still build") {
    if (!Kernel::Available()) { std::printf("    (skipped: no geometry kernel)\n"); return; }
    Document d;
    Sketch open;
    open.AddLine(0, 0, 10, 0);
    open.AddLine(10, 0, 10, 10);
    const Id sk = d.AddFeature(SketchFeature(open));
    const Id ex = d.AddFeature(ExtrudeFeature(sk, "5"));
    Feature fil; fil.type = FeatureType::Fillet; fil.body = ex;
    const Id fl = d.AddFeature(fil);
    Feature box; box.type = FeatureType::Box;
    const Id bx = d.AddFeature(box);
    const RegenerateResult r = d.Regenerate();
    CHECK(!r.ok);
    CHECK(r.failed == 1);
    CHECK(r.skipped == 1);
    CHECK(d.State(ex)->error.find("open") != std::string::npos);
    CHECK(d.State(fl)->skipped);
    CHECK(d.State(bx)->built);
    CHECK(d.Bodies().size() == 1);
    // bad expressions and values
    Feature b = *d.FindFeature(bx); b.values["x"] = "nope * 2";
    CHECK(d.UpdateFeature(b));
    d.Regenerate();
    CHECK(d.State(bx)->error.find("nope") != std::string::npos);
    b.values["x"] = "-3";
    CHECK(d.UpdateFeature(b));
    d.Regenerate();
    CHECK(!d.State(bx)->built);
    // suppressing removes a feature from the build without deleting it
    b.values["x"] = "10"; b.suppressed = true;
    CHECK(d.UpdateFeature(b));
    d.Regenerate();
    CHECK(d.Bodies().empty());
    // rollback builds only the start of the history
    b.suppressed = false;
    CHECK(d.UpdateFeature(b));
    d.rollback = 0;
    d.Regenerate();
    CHECK(d.Bodies().empty());
    d.rollback = size_t(-1);
    d.Regenerate();
    CHECK(d.Bodies().size() == 1);
}

TEST_CASE("CAD kernel: measurement, tessellation, STEP / IGES / STL / BREP exchange, units") {
    if (!Kernel::Available()) { std::printf("    (skipped: no geometry kernel)\n"); return; }
    TempDir tmp;
    Document d;   // millimetres
    Feature box; box.type = FeatureType::Box; box.values = { { "x", "30" }, { "y", "20" }, { "z", "10" } };
    d.AddFeature(box);
    Feature sph; sph.type = FeatureType::Sphere; sph.position = { 100, 0, 0 }; sph.values["radius"] = "5";
    d.AddFeature(sph);
    CHECK(d.Regenerate().ok);
    double dist = 0;
    glm::dvec3 pa, pb;
    CHECK(Kernel::Distance(d.Bodies()[0].shape, d.Bodies()[1].shape, dist, &pa, &pb));
    CHECK_NEAR(dist, 100 - 5 - 30, 1e-6);
    CHECK(Kernel::DistanceToPoint(d.Bodies()[0].shape, { 30, 10, 25 }, dist));
    CHECK_NEAR(dist, 15, 1e-6);
    const std::vector<FaceInfo> faces = Kernel::FacesOf(d.Bodies()[1].shape);
    CHECK(!faces.empty() && faces[0].surface == SurfaceType::Sphere);
    CHECK(!faces.empty() && std::fabs(faces[0].radius - 5) < 1e-9);
    const Tessellation t = Kernel::Tessellate(d.Result(), 0.05);
    CHECK(!t.indices.empty());
    CHECK(t.indices.size() == t.faceOf.size() * 3);
    CHECK(t.positions.size() == t.normals.size());
    CHECK(t.edges.size() >= 12);

    // STEP: exact solids survive the round trip
    const std::string step = (tmp.path / "part.step").string();
    std::string error;
    CHECK(d.ExportResult(step, FileFormat::Step, &error));
    Shape back = Document::ImportFile(step, Unit::Millimetre, &error);
    CHECK(back);
    const Properties bp = Kernel::Measure(back);
    CHECK_NEAR(bp.volume, 6000 + 4.0 / 3.0 * PI * 125, 1e-3);
    CHECK(bp.solids == 2);
    // IGES
    const std::string iges = (tmp.path / "part.igs").string();
    CHECK(d.ExportResult(iges, FileFormat::Iges, &error));
    Shape ib = Document::ImportFile(iges, Unit::Millimetre, &error);
    CHECK(ib);
    CHECK(Kernel::Measure(ib).faces == Kernel::Measure(d.Result()).faces);
    // STL: triangles only
    const std::string stl = (tmp.path / "part.stl").string();
    CHECK(d.ExportResult(stl, FileFormat::Stl, &error));
    CHECK(fs::exists(stl) && fs::file_size(stl) > 84);
    // units: a centimetre document reads the millimetre STEP as 1/10 the size
    Shape cm = Document::ImportFile(step, Unit::Centimetre, &error);
    CHECK_NEAR(Kernel::Measure(cm).volume, (6000 + 4.0 / 3.0 * PI * 125) / 1000.0, 1e-6);
    // an Import feature keeps the exact geometry in the document (BREP) and reopens without the file
    Document imp;
    Feature fi; fi.type = FeatureType::Import; fi.asset = "asset:0000000000000001/part.step"; fi.brep = Kernel::ToBrep(back);
    const Id fid = imp.AddFeature(fi);
    Feature fl; fl.type = FeatureType::Fillet; fl.body = fid; fl.values["radius"] = "1";
    imp.AddFeature(fl);
    Document reopened;
    CHECK(Document::FromJson(imp.ToJson(), reopened, &error));
    fs::remove(step);
    const RegenerateResult rr = reopened.Regenerate();
    CHECK(reopened.State(fid)->built);
    CHECK(reopened.Bodies().size() == 1);   // the imported compound is one body
    (void)rr;
    // an Import without cached geometry resolves its asset through the resolver
    Document viaAsset;
    Feature fa; fa.type = FeatureType::Import; fa.asset = "asset:x/part.igs";
    viaAsset.AddFeature(fa);
    CHECK(viaAsset.Regenerate([&](const std::string& ref) { return ref == "asset:x/part.igs" ? iges : std::string(); }).ok);
    CHECK(!viaAsset.Regenerate([](const std::string&) { return std::string(); }).ok);
    // unreadable files
    CHECK(!Document::ImportFile((tmp.path / "missing.step").string(), Unit::Millimetre, &error));
    { std::ofstream(tmp.path / "junk.step") << "ISO-10303-21; garbage"; }
    CHECK(!Document::ImportFile((tmp.path / "junk.step").string(), Unit::Millimetre, &error));
    CHECK(!Document::ImportFile((tmp.path / "x.dwg").string(), Unit::Millimetre, &error));
    // brep text round trip
    CHECK(Kernel::FromBrep(Kernel::ToBrep(d.Result())));
    CHECK(!Kernel::FromBrep("garbage", &error));
}

// ============================================================================ scene component
#include "Phisiks/Scene/Scene.h"
#include "Phisiks/Scene/SceneSerializer.h"

TEST_CASE("CAD: cadModel component in scene files (document as an object, validated, revision)") {
    Phisiks::Scene scene("cad");
    Phisiks::Entity e = scene.CreateEntity("Part");
    Document d;
    d.SetParameter("w", "12");
    Feature box; box.type = FeatureType::Box; box.values = { { "x", "w" }, { "y", "2" }, { "z", "3" } };
    d.AddFeature(box);
    Phisiks::CadModel cad;
    cad.document = d.ToJson();
    e.Add<Phisiks::CadModel>(cad);
    const std::string json = Phisiks::SceneSerializer::ToJson(scene);
    CHECK(json.find("\"cadModel\"") != std::string::npos && json.find("\"phisiks-cad\"") != std::string::npos && json.find("\\\"format") == std::string::npos);   // an object, not an escaped string
    Phisiks::Scene back("cad");
    CHECK(Phisiks::SceneSerializer::FromJson(back, json));
    Phisiks::Entity b = back.FindByGuid(e.GetGuid());
    CHECK(b && b.Has<Phisiks::CadModel>());
    Document reread;
    CHECK(b && Document::FromJson(b.Get<Phisiks::CadModel>().document, reread));
    CHECK(reread.ToJson() == d.ToJson());
    // a changed document bumps the revision (the editor rebuilds); an equal one does not
    const uint64_t rev = b.Get<Phisiks::CadModel>().revision;
    CHECK(Phisiks::SceneSerializer::SetComponentJson(b, "cadModel", "{\"document\":" + d.ToJson() + "}"));
    CHECK(b.Get<Phisiks::CadModel>().revision == rev);
    d.SetParameter("w", "20");
    CHECK(Phisiks::SceneSerializer::SetComponentJson(b, "cadModel", "{\"document\":" + d.ToJson() + "}"));
    CHECK(b.Get<Phisiks::CadModel>().revision == rev + 1);
    std::string error;
    CHECK(!Phisiks::SceneSerializer::SetComponentJson(b, "cadModel", "{\"document\":{\"format\":\"other\"}}", &error));
    CHECK(!Phisiks::SceneSerializer::SetComponentJson(b, "cadModel", "{\"document\":\"text\"}", &error));
    CHECK(Phisiks::CadModel::MeshName(0x1234) == "cad:0000000000001234");
}
