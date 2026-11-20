// Headless renderer tests: shader-cache file format and atomic writes, GLSL preprocessing and the
// embedded program set, primitive meshes, BRDF / falloff / fog reference math, shadow view fitting,
// frustum culling and the built-in font. No GL context is needed.
#include "TestFramework.h"

#include "render/RenderMath.h"
#include "render/ShaderCacheFormat.h"
#include "render/ShaderPreprocessor.h"
#include "render/ShaderPrograms.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <bit>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>

using namespace Phisiks;
using namespace Phisiks::Render;
namespace Cache = Phisiks::Render::ShaderCacheFormat;
namespace RM = Phisiks::Render::RenderMath;

namespace {
    Cache::ProgramBlob SampleBlob(uint64_t key) {
        Cache::ProgramBlob blob;
        blob.key = key;
        blob.binaryFormat = 0x8E21;
        for (int i = 0; i < 300; ++i) blob.binary.push_back(uint8_t(i * 7 + 3));
        return blob;
    }

    std::filesystem::path TempDir() {
        auto dir = std::filesystem::temp_directory_path() / "phisiks_render_tests";
        std::filesystem::create_directories(dir);
        return dir;
    }
}

// ============================================================================ shader cache format
TEST_CASE("ShaderCache: serialize/parse round trip") {
    const Cache::ProgramBlob blob = SampleBlob(0x1234);
    const std::vector<uint8_t> bytes = Cache::Serialize(blob);
    CHECK(bytes.size() == Cache::HEADER_SIZE + blob.binary.size());
    Cache::ProgramBlob out;
    CHECK(Cache::Parse(bytes.data(), bytes.size(), 0x1234, out) == Cache::ParseResult::Ok);
    CHECK(out.binaryFormat == blob.binaryFormat && out.binary == blob.binary);
}

TEST_CASE("ShaderCache: corrupt, truncated, stale and foreign files are rejected") {
    const std::vector<uint8_t> good = Cache::Serialize(SampleBlob(42));
    Cache::ProgramBlob out;
    CHECK(Cache::Parse(good.data(), 10, 42, out) == Cache::ParseResult::TooSmall);
    CHECK(Cache::Parse(good.data(), good.size(), 43, out) == Cache::ParseResult::KeyMismatch);
    std::vector<uint8_t> truncated(good.begin(), good.end() - 1);
    CHECK(Cache::Parse(truncated.data(), truncated.size(), 42, out) == Cache::ParseResult::BadSize);
    std::vector<uint8_t> flipped = good;
    flipped.back() ^= 0x40;   // one bit in the driver binary
    CHECK(Cache::Parse(flipped.data(), flipped.size(), 42, out) == Cache::ParseResult::BadChecksum);
    std::vector<uint8_t> magic = good;
    magic[0] = 'X';
    CHECK(Cache::Parse(magic.data(), magic.size(), 42, out) == Cache::ParseResult::BadMagic);
    std::vector<uint8_t> version = good;
    version[4] = 99;
    CHECK(Cache::Parse(version.data(), version.size(), 42, out) == Cache::ParseResult::BadVersion);
    Cache::ProgramBlob empty;
    empty.key = 42;
    const std::vector<uint8_t> emptyBytes = Cache::Serialize(empty);
    CHECK(Cache::Parse(emptyBytes.data(), emptyBytes.size(), 42, out) == Cache::ParseResult::EmptyBinary);
}

TEST_CASE("ShaderCache: key depends on every source byte, stage boundaries and the driver") {
    const uint64_t base = Cache::ComputeProgramKey({ "void main(){}", "out vec4 c;" }, "Intel|UHD|4.6");
    CHECK(base == Cache::ComputeProgramKey({ "void main(){}", "out vec4 c;" }, "Intel|UHD|4.6"));
    CHECK(base != Cache::ComputeProgramKey({ "void main(){ }", "out vec4 c;" }, "Intel|UHD|4.6"));
    CHECK(base != Cache::ComputeProgramKey({ "void main(){}", "out vec4 c;" }, "Intel|UHD|4.6.1"));   // driver update
    CHECK(base != Cache::ComputeProgramKey({ "void main(){}out vec4 c;" }, "Intel|UHD|4.6"));          // same text, one stage
    CHECK(Cache::KeyToFileName(0xABCull) == "0000000000000abc.phsc");
}

TEST_CASE("ShaderCache: atomic write replaces files and leaves no temp files") {
    const auto dir = TempDir() / "atomic";
    std::filesystem::remove_all(dir);
    const auto file = dir / "a.phsc";
    CHECK(Cache::WriteFileAtomic(file, { 1, 2, 3 }));
    CHECK(Cache::WriteFileAtomic(file, { 9, 8, 7, 6 }));   // overwrite
    std::vector<uint8_t> read;
    CHECK(Cache::ReadFile(file, read));
    CHECK((read == std::vector<uint8_t>{ 9, 8, 7, 6 }));
    int files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) { (void)entry; ++files; }
    CHECK(files == 1);
    CHECK(!Cache::ReadFile(dir / "missing.phsc", read));
    std::filesystem::remove_all(dir);
}

// ============================================================================ preprocessing
TEST_CASE("ShaderPreprocessor: includes (nested, once), defines after #version") {
    std::map<std::string, std::string> files = {
        { "a.glsl", "#include \"b.glsl\"\nfloat A;\n" },
        { "b.glsl", "float B;\n" },
    };
    auto resolve = [&](const std::string& n) -> const std::string* { auto it = files.find(n); return it == files.end() ? nullptr : &it->second; };
    const auto r = ShaderPreprocessor::Expand("#version 460 core\r\n#include \"a.glsl\"\n#include \"b.glsl\"\nvoid main(){}\n", resolve, { "FOO 1" });
    CHECK(r.ok);
    CHECK(r.source.find("#version 460 core\n#define FOO 1\n") == 0);
    CHECK(r.source.find("float B;") < r.source.find("float A;"));
    CHECK(r.source.find("float B;") == r.source.rfind("float B;"));   // included once
    CHECK(r.source.find('\r') == std::string::npos);
}

TEST_CASE("ShaderPreprocessor: unknown and recursive includes fail with a message") {
    std::map<std::string, std::string> files = { { "x", "#include \"y\"\n" }, { "y", "#include \"x\"\n" } };
    auto resolve = [&](const std::string& n) -> const std::string* { auto it = files.find(n); return it == files.end() ? nullptr : &it->second; };
    auto unknown = ShaderPreprocessor::Expand("#include \"nope\"\n", resolve);
    CHECK(!unknown.ok && unknown.error.find("nope") != std::string::npos);
    auto recursive = ShaderPreprocessor::Expand("#include \"x\"\n", resolve);
    CHECK(!recursive.ok && recursive.error.find("recursive") != std::string::npos);
}

TEST_CASE("Shaders: every program's stages are embedded and preprocess") {
    const auto resolve = [](const std::string& n) { return Shaders::Find(n); };
    CHECK(Shaders::Programs().size() == size_t(Shaders::PROGRAM_COUNT));
    std::set<std::string> used;
    for (const Shaders::ProgramDef& program : Shaders::Programs()) {
        for (const Shaders::StageDef& stage : program.stages) {
            const std::string* source = Shaders::Find(stage.file);
            CHECK(source != nullptr);
            if (!source) continue;
            const auto r = ShaderPreprocessor::Expand(*source, resolve, program.defines);
            CHECK(r.ok);
            CHECK(r.source.rfind("#version 460 core", 0) == 0);
            CHECK(r.source.find("#include") == std::string::npos);
            used.insert(stage.file);
        }
    }
    CHECK(used.size() == 10);              // stage files (plus 5 shared includes)
    CHECK(Shaders::FileNames().size() == 15);
}

// ============================================================================ primitives
TEST_CASE("Primitives: unit cube, sphere and plane with outward counter-clockwise faces") {
    auto checkWinding = [](const MeshData& mesh) {
        int bad = 0;
        for (size_t i = 0; i < mesh.indices.size(); i += 3) {
            const Vertex& a = mesh.vertices[mesh.indices[i]];
            const Vertex& b = mesh.vertices[mesh.indices[i + 1]];
            const Vertex& c = mesh.vertices[mesh.indices[i + 2]];
            const glm::vec3 faceNormal = glm::cross(b.position - a.position, c.position - a.position);
            if (glm::dot(faceNormal, a.normal + b.normal + c.normal) <= 0.0f) ++bad;
        }
        return bad;
    };
    const MeshData cube = RM::MakeCube();
    CHECK(cube.vertices.size() == 24 && cube.indices.size() == 36);
    CHECK(checkWinding(cube) == 0);
    for (const Vertex& v : cube.vertices) {
        CHECK_NEAR(glm::length(v.normal), 1.0, 1e-6);
        CHECK_NEAR(glm::dot(v.position, v.normal), 0.5, 1e-6);   // on the face plane, 0.5 m out
    }
    CHECK_NEAR(RM::BoundingRadius(cube), std::sqrt(0.75), 1e-6);

    const MeshData sphere = RM::MakeSphere(32, 16);
    CHECK(checkWinding(sphere) == 0);
    for (const Vertex& v : sphere.vertices) CHECK_NEAR(glm::length(v.position), 0.5, 1e-5);

    const MeshData plane = RM::MakePlane();
    CHECK(plane.indices.size() == 6 && checkWinding(plane) == 0);
    CHECK(plane.vertices[0].normal == glm::vec3(0, 1, 0));
}

// ============================================================================ BRDF / light falloff
TEST_CASE("BRDF: GGX distribution is normalised (integral of D(h) cos = 1)") {
    for (float alpha : { 0.2f, 0.5f, 1.0f }) {
        // Integrate over the hemisphere in (theta, phi); D depends on theta only.
        const int steps = 4000;
        double sum = 0.0;
        for (int i = 0; i < steps; ++i) {
            const double theta = (i + 0.5) / steps * glm::half_pi<double>();
            sum += RM::D_GGX(float(std::cos(theta)), alpha) * std::cos(theta) * std::sin(theta);
        }
        sum *= glm::half_pi<double>() / steps * glm::two_pi<double>();
        CHECK_NEAR(sum, 1.0, 0.01);
    }
}

TEST_CASE("BRDF: Fresnel endpoints, symmetric visibility, white-furnace bound") {
    const glm::vec3 f0(0.04f);
    CHECK_NEAR(RM::F_Schlick(1.0f, f0).x, 0.04, 1e-6);
    CHECK_NEAR(RM::F_Schlick(0.0f, f0).x, 1.0, 1e-6);
    CHECK_NEAR(RM::V_SmithGGXCorrelated(0.3f, 0.8f, 0.4f), RM::V_SmithGGXCorrelated(0.8f, 0.3f, 0.4f), 1e-7);
    // White furnace (F = 1): directional albedo of the specular lobe. Single-scattering GGX never
    // creates energy (<= 1), is nearly lossless when smooth and loses energy with roughness
    // (independent reference integration: 0.981 / 0.683 / 0.379 at alpha 0.1 / 0.5 / 1.0, NoV 0.7).
    const double expected[3] = { 0.981, 0.683, 0.379 };
    int k = 0;
    for (float alpha : { 0.1f, 0.5f, 1.0f }) {
        const float NoV = 0.7f;
        const glm::vec3 V(std::sqrt(1.0f - NoV * NoV), 0.0f, NoV);
        double albedo = 0.0;
        const int n = 300;
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                const double theta = (i + 0.5) / n * glm::half_pi<double>(), phi = (j + 0.5) / n * glm::two_pi<double>();
                const glm::vec3 L(float(std::sin(theta) * std::cos(phi)), float(std::sin(theta) * std::sin(phi)), float(std::cos(theta)));
                const glm::vec3 H = glm::normalize(V + L);
                const double f = RM::D_GGX(H.z, alpha) * RM::V_SmithGGXCorrelated(NoV, L.z, alpha);
                albedo += f * L.z * std::sin(theta);
            }
        }
        albedo *= (glm::half_pi<double>() / n) * (glm::two_pi<double>() / n);
        CHECK(albedo <= 1.0 + 1e-3);
        CHECK_NEAR(albedo, expected[k++], 0.01);
    }
}

TEST_CASE("Light falloff: inverse square inside, smooth zero at range; spot cone") {
    CHECK_NEAR(RM::DistanceAttenuation(1.0f, 100.0f), 1.0, 1e-6);
    CHECK_NEAR(RM::DistanceAttenuation(2.0f, 100.0f) / RM::DistanceAttenuation(1.0f, 100.0f), 0.25, 1e-5);
    CHECK(RM::DistanceAttenuation(10.0f, 10.0f) == 0.0f);
    CHECK(RM::DistanceAttenuation(12.0f, 10.0f) == 0.0f);
    float previous = 1e9f;
    for (float d = 0.5f; d < 10.0f; d += 0.25f) {   // strictly decreasing
        const float a = RM::DistanceAttenuation(d, 10.0f);
        CHECK(a < previous);
        previous = a;
    }
    const float ci = std::cos(0.3f), co = std::cos(0.5f);
    CHECK(RM::SpotFactor(std::cos(0.1f), ci, co) == 1.0f);
    CHECK(RM::SpotFactor(std::cos(0.6f), ci, co) == 0.0f);
    const float mid = RM::SpotFactor(std::cos(0.4f), ci, co);
    CHECK(mid > 0.0f && mid < 1.0f);
}

// ============================================================================ fog
TEST_CASE("Fog: closed-form optical depth equals numeric integration") {
    FogSettings fog;
    fog.distanceDensity = 0.01f;
    fog.heightDensity = 0.08f;
    fog.heightFalloff = 0.4f;
    fog.heightBase = 0.5f;
    const glm::vec3 origin(0.0f, 1.7f, 0.0f);
    for (const glm::vec3& d : { glm::vec3(1, 0, 0), glm::normalize(glm::vec3(1, 0.3f, 0)), glm::normalize(glm::vec3(1, -0.08f, 0.2f)), glm::vec3(0, 1, 0) }) {
        const float L = 35.0f;
        double numeric = 0.0;
        const int n = 20000;
        for (int i = 0; i < n; ++i) {
            const double t = (i + 0.5) / n * L;
            const double y = origin.y + d.y * t;
            numeric += fog.distanceDensity + fog.heightDensity * std::exp(-fog.heightFalloff * (y - fog.heightBase));
        }
        numeric *= L / n;
        CHECK_NEAR(RM::FogOpticalDepth(origin, d, L, fog), numeric, numeric * 1e-3);
    }
    CHECK(RM::FogAmount(origin, glm::vec3(1, 0, 0), 1e6f, fog) == fog.maxOpacity);
    fog.enabled = false;
    CHECK(RM::FogAmount(origin, glm::vec3(1, 0, 0), 50.0f, fog) == 0.0f);
}

// ============================================================================ shadow views / culling
TEST_CASE("Shadows: directional matrix encloses the camera slice and snaps to texels") {
    const glm::vec3 lightDir = glm::normalize(glm::vec3(-0.45f, -1.0f, -0.35f));
    const glm::quat camRot = glm::angleAxis(0.4f, glm::vec3(0, 1, 0)) * glm::angleAxis(-0.2f, glm::vec3(1, 0, 0));
    const int size = 2048;
    const auto corners = RM::FrustumCorners(glm::vec3(3, 1.6f, 4), camRot, 1.0f, 16.0f / 9.0f, 0.05f, 40.0f);
    const glm::mat4 m = RM::DirectionalShadowMatrix(lightDir, corners, size);
    for (const glm::vec3& c : corners) {
        const glm::vec4 p = m * glm::vec4(c, 1.0f);
        CHECK(std::abs(p.x) <= 1.0001f && std::abs(p.y) <= 1.0001f && p.z >= -1.0001f && p.z <= 1.0001f);
    }
    // A point 30 m towards the light from the slice centre (an off-screen caster) is still inside.
    glm::vec3 center(0.0f);
    for (const glm::vec3& c : corners) center += c / 8.0f;
    const glm::vec4 caster = m * glm::vec4(center - lightDir * 30.0f, 1.0f);
    CHECK(caster.z >= -1.0f && caster.z <= 1.0f);

    // Moving the camera slightly shifts the projection by whole texels only (no shimmer).
    const auto moved = RM::FrustumCorners(glm::vec3(3.013f, 1.6f, 4.007f), camRot, 1.0f, 16.0f / 9.0f, 0.05f, 40.0f);
    const glm::mat4 m2 = RM::DirectionalShadowMatrix(lightDir, moved, size);
    const glm::vec4 a = m * glm::vec4(0, 0, 0, 1), b = m2 * glm::vec4(0, 0, 0, 1);
    const float texelsX = (b.x - a.x) * size * 0.5f, texelsY = (b.y - a.y) * size * 0.5f;
    CHECK_NEAR(texelsX, std::round(texelsX), 1e-2);
    CHECK_NEAR(texelsY, std::round(texelsY), 1e-2);
    CHECK_NEAR(m[0][0], m2[0][0], 1e-7);   // same extent (radius quantized)
}

TEST_CASE("Shadows: spot and point light views look the right way") {
    const glm::vec3 p(1, 4, 2);
    const glm::mat4 spot = RM::SpotShadowMatrix(p, glm::vec3(0, -1, 0), 0.5f, 10.0f);
    const glm::vec4 below = spot * glm::vec4(p + glm::vec3(0, -3, 0), 1.0f);
    CHECK(std::abs(below.x / below.w) < 1e-4f && std::abs(below.y / below.w) < 1e-4f && below.z / below.w < 1.0f);
    const auto faces = RM::PointShadowMatrices(p, 0.05f, 10.0f);
    const glm::vec3 axes[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (int f = 0; f < 6; ++f) {
        const glm::vec4 c = faces[f] * glm::vec4(p + axes[f] * 3.0f, 1.0f);
        CHECK(c.w > 0.0f && std::abs(c.x / c.w) < 1e-4f && std::abs(c.y / c.w) < 1e-4f);
    }
}

TEST_CASE("Culling: frustum sphere test") {
    const glm::mat4 vp = glm::perspective(1.0f, 1.0f, 0.1f, 100.0f) * glm::lookAt(glm::vec3(0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
    const RM::Frustum f = RM::ExtractFrustum(vp);
    CHECK(f.IntersectsSphere(glm::vec3(0, 0, -10), 1.0f));
    CHECK(!f.IntersectsSphere(glm::vec3(0, 0, 10), 1.0f));          // behind
    CHECK(!f.IntersectsSphere(glm::vec3(0, 0, -200), 1.0f));        // beyond far
    CHECK(!f.IntersectsSphere(glm::vec3(50, 0, -10), 1.0f));        // outside left/right
    CHECK(f.IntersectsSphere(glm::vec3(6.0f, 0, -10), 2.0f));       // straddles the edge
}

// ============================================================================ font
TEST_CASE("Font: every printable glyph has pixels, atlas layout, measuring") {
    const std::vector<uint8_t> atlas = RM::Font::BuildAtlas();
    CHECK(atlas.size() == size_t(RM::Font::ATLAS_W * RM::Font::ATLAS_H));
    for (int c = 33; c < 127; ++c) {
        int lit = 0;
        for (int y = 0; y < RM::Font::GLYPH_H; ++y) lit += std::popcount(unsigned(RM::Font::Glyph(char(c))[y]));
        CHECK(lit > 0);
    }
    for (int y = 0; y < RM::Font::GLYPH_H; ++y) CHECK(RM::Font::Glyph(' ')[y] == 0);
    CHECK(RM::Font::Glyph('\x01') == RM::Font::Glyph('?'));
    // 'O' and '0' must differ (the zero has a slash).
    bool differ = false;
    for (int y = 0; y < RM::Font::GLYPH_H; ++y) differ |= RM::Font::Glyph('O')[y] != RM::Font::Glyph('0')[y];
    CHECK(differ);
    const glm::vec2 size = RM::Font::Measure("ab\ncdef", 2.0f);
    CHECK_NEAR(size.x, 4 * RM::Font::ADVANCE * 2.0, 1e-6);
    CHECK_NEAR(size.y, 2 * RM::Font::LINE_HEIGHT * 2.0, 1e-6);
}
