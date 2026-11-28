#pragma once
// Phisiks::Animation: glTF 2.0 import (.gltf + .bin / data URIs, or binary .glb), skeletal
// animation clips, dual-quaternion skinning (Phisiks::Math::DualQuat) and spline camera paths.
// Clean-room implementation of the Khronos glTF 2.0 specification.
//
//   Model model;
//   if (Animation::LoadGltf("models/walker.glb", model, &error)) { ... }
//   entity.Add<SkinnedModel>().path = "models/walker.glb";   // the scene system loads, animates, skins
//
// Skinning runs on the CPU (all renderer tiers, incl. OpenGL 4.1) and uploads the deformed vertices
// with Render::UpdateMeshVertices.

#include "Phisiks/Math/DualQuat.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/Components.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Phisiks {
    class Scene;
}

namespace Phisiks::Animation {

    struct Node {
        std::string name;
        int parent = -1;
        std::vector<int> children;
        Transform local;                       // rest pose (TRS; a matrix is decomposed)
        int mesh = -1;                         // index into Model::meshes
        int skin = -1;                         // index into Model::skins
    };

    struct Primitive {
        std::vector<Render::Vertex> vertices;  // bind pose (mesh space)
        std::vector<uint32_t> indices;         // triangle list
        std::vector<glm::uvec4> joints;        // JOINTS_0 (indices into the skin's joint list); empty = rigid
        std::vector<glm::vec4> weights;        // WEIGHTS_0 (normalised)
        int material = -1;
    };
    struct Mesh { std::string name; std::vector<Primitive> primitives; };

    struct Skin {
        std::string name;
        std::vector<int> joints;               // node indices
        std::vector<glm::mat4> inverseBind;    // per joint
    };

    struct Material {
        std::string name;
        glm::vec4 baseColor{ 1.0f };           // linear
        float metallic = 1.0f, roughness = 1.0f;
        glm::vec3 emissive{ 0.0f };
        int baseColorImage = -1;               // index into Model::images
        bool doubleSided = false;
    };

    struct Image { int width = 0, height = 0; std::vector<uint8_t> rgba; std::string name; };

    enum class Interpolation { Step, Linear, CubicSpline };
    enum class TargetPath { Translation, Rotation, Scale };
    struct Channel {
        int node = -1;
        TargetPath path = TargetPath::Translation;
        Interpolation interpolation = Interpolation::Linear;
        std::vector<float> times;              // seconds, ascending
        std::vector<glm::vec4> values;         // xyz (T/S) or xyzw quaternion (R); cubic: in-tangent, value, out-tangent per key
    };
    struct Clip {
        std::string name;
        float duration = 0.0f;
        std::vector<Channel> channels;
    };

    struct Model {
        std::vector<Node> nodes;
        std::vector<int> roots;                // scene root nodes
        std::vector<Mesh> meshes;
        std::vector<Skin> skins;
        std::vector<Material> materials;
        std::vector<Image> images;
        std::vector<Clip> clips;
        int FindNode(const std::string& name) const;
        int FindClip(const std::string& name) const;
    };

    // Loads .gltf (JSON, external .bin / images next to it or data: URIs) or .glb (binary container).
    bool LoadGltf(const std::string& assetPath, Model& out, std::string* error = nullptr);
    // From memory; `baseDirectory` resolves relative URIs ("" = only embedded data).
    bool ParseGltf(const std::vector<uint8_t>& bytes, const std::string& baseDirectory, Model& out, std::string* error = nullptr);

    // ---- pose evaluation
    // Local TRS of every node at `time` (seconds; wrapped when `loop`). Unanimated nodes keep their rest pose.
    void SamplePose(const Model& model, const Clip* clip, float time, bool loop, std::vector<Transform>& nodeLocals);
    // Model-space matrices from local TRS (parents before children are not required).
    void GlobalMatrices(const Model& model, const std::vector<Transform>& nodeLocals, std::vector<glm::mat4>& out);
    // Skinning transforms per skin joint: global(joint) * inverseBind, as unit dual quaternions
    // (rigid part) plus the matrices (for linear blend skinning).
    void SkinTransforms(const Model& model, int skin, const std::vector<glm::mat4>& globals,
                        std::vector<Math::DualQuat>& dualQuats, std::vector<glm::mat4>& matrices);

    enum class SkinningMethod { DualQuaternion, Linear };
    // Deforms a skinned primitive (bind pose -> current pose). Normals follow the rotation.
    void SkinVertices(const Primitive& primitive, const std::vector<Math::DualQuat>& dualQuats, const std::vector<glm::mat4>& matrices,
              SkinningMethod method, std::vector<Render::Vertex>& out);

    // ---- spline camera paths
    enum class SplineType { CatmullRom, Bezier };
    // Centripetal Catmull-Rom (alpha 0.5: no cusps or self-intersections inside a segment).
    glm::vec3 CatmullRom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, float t, float alpha = 0.5f);
    glm::vec3 CubicBezier(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, float t);

    // Arc-length parameterised spline. Catmull-Rom passes through every point; Bezier takes
    // segments of four control points sharing their end points (p0 c c p1 c c p2 ...).
    class Spline {
    public:
        bool Build(SplineType type, const std::vector<glm::vec3>& points, bool closed = false, int samplesPerSegment = 32);
        glm::vec3 Evaluate(float u) const;     // u in [0, 1] along the length (constant speed)
        glm::vec3 Tangent(float u) const;      // unit
        float Length() const { return m_length; }
        bool Valid() const { return m_segments > 0; }
    private:
        glm::vec3 Raw(float s) const;          // s in [0, segments]
        SplineType m_type = SplineType::CatmullRom;
        std::vector<glm::vec3> m_points;
        bool m_closed = false;
        int m_segments = 0;
        std::vector<float> m_arc;              // cumulative length per sample
        float m_length = 0.0f;
    };
}

namespace Phisiks {
    // Imported glTF model, animated and skinned by the scene's animation system. Meshes appear as
    // child entities ("<entity>/<mesh>") carrying MeshRenderers.
    struct SkinnedModel {
        std::string path;                      // asset path (.gltf / .glb)
        std::string clip;                      // clip name; empty = first clip
        float speed = 1.0f;
        float time = 0.0f;
        bool loop = true;
        bool playing = true;
        Animation::SkinningMethod method = Animation::SkinningMethod::DualQuaternion;
        // runtime
        bool loaded = false;
        std::string error;
        uint32_t vertexCount = 0;              // skinned vertices updated per frame
    };

    // Moves the entity (normally the camera) along a spline, looking at `lookAt` points (same
    // count as `points`, interpolated with the same spline) or along the path.
    struct CameraPath {
        Animation::SplineType type = Animation::SplineType::CatmullRom;
        std::vector<glm::vec3> points;
        std::vector<glm::vec3> lookAt;
        float duration = 10.0f;                // seconds for the whole path
        float time = 0.0f;
        bool loop = true;
        bool closed = false;
        bool playing = true;
        // runtime
        Animation::Spline positionSpline, targetSpline;
        bool built = false;
    };

    namespace Animation {
        // Scene system (the Application calls it): loads / animates / skins SkinnedModels and moves CameraPaths.
        void UpdateScene(Scene& scene, float deltaTime);
        void UpdateCameraPaths(Scene& scene, float deltaTime);
        struct Stats { uint32_t models = 0, skinnedVertices = 0, clipsPlaying = 0, cameraPaths = 0; float skinningMs = 0.0f; };
        Stats GetStats();
        void ClearCache();                      // forget loaded models (meshes stay registered)
    }
}
