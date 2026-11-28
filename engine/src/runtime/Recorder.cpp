#include "runtime/Recorder.h"
#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

namespace Phisiks {
    namespace {
        void FlipRows(const uint8_t* src, int width, int height, int bytesPerPixel, std::vector<uint8_t>& dst) {
            const size_t row = size_t(width) * bytesPerPixel;
            dst.resize(row * height);
            for (int y = 0; y < height; ++y) std::copy_n(src + size_t(height - 1 - y) * row, row, dst.data() + size_t(y) * row);
        }
        bool WritePfm(const std::string& path, const std::vector<float>& values, int width, int height) {
            std::FILE* f = std::fopen(path.c_str(), "wb");
            if (!f) return false;
            std::fprintf(f, "Pf\n%d %d\n-1.0\n", width, height);   // greyscale, little-endian, rows bottom-up (as stored)
            std::fwrite(values.data(), sizeof(float), values.size(), f);
            std::fclose(f);
            return true;
        }
    }

    bool Recorder::Begin(const std::string& directory, bool frames, bool data, int every, bool vision) {
        End();
        m_directory = directory;
        m_frames = frames;
        m_data = data;
        m_vision = vision;
        m_visionWritten = 0;
        m_seen.clear();
        m_every = std::max(every, 1);
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (m_data) {
            m_csv = std::fopen((directory + "/sim_data.csv").c_str(), "wb");
            if (!m_csv) {
                Log::Error() << "Recorder: cannot write " << directory << "/sim_data.csv";
                return false;
            }
            std::fputs("frame,step,sim_time,entity,name,px,py,pz,qx,qy,qz,qw,vx,vy,vz\n", m_csv);
        }
        if (m_vision) {
            std::filesystem::create_directories(directory + "/vision", ec);
            m_cameras = std::fopen((directory + "/vision/cameras.csv").c_str(), "wb");
            if (m_cameras) std::fputs("frame,step,sim_time,width,height,fx,fy,cx,cy,near,far,orthographic,view00,view01,view02,view03,view10,view11,view12,view13,view20,view21,view22,view23,view30,view31,view32,view33\n", m_cameras);
        }
        m_active = true;
        Log::Info() << "Recorder: exporting" << (frames ? " PNG frames" : "") << (data ? " CSV data" : "") << (vision ? " vision passes" : "") << " every " << m_every << " frame(s) to " << directory;
        return true;
    }

    void Recorder::Capture(Scene* scene, uint64_t frameIndex, uint64_t totalSteps, double simulationTime, bool rendered) {
        if (!m_active || frameIndex % uint64_t(m_every) != 0) return;
        if (m_frames) {
            Render::FrameCapture capture;
            if (rendered && Render::CaptureFrame(capture)) {
                Assets::Image image;
                image.width = capture.width;
                image.height = capture.height;
                image.rgba.resize(capture.rgba.size());
                const size_t row = size_t(capture.width) * 4;
                for (int y = 0; y < capture.height; ++y) {   // GL rows are bottom-up
                    std::copy_n(capture.rgba.data() + size_t(capture.height - 1 - y) * row, row, image.rgba.data() + size_t(y) * row);
                }
                char name[64];
                std::snprintf(name, sizeof(name), "/frame_%06llu.png", static_cast<unsigned long long>(frameIndex));
                if (Assets::WritePng(m_directory + name, image)) ++m_framesWritten;
            }
            else if (!m_warnedNoRender) {
                m_warnedNoRender = true;
                Log::Warning() << "Recorder: PNG frames need rendering (not available in headless mode)";
            }
        }
        if (m_vision && rendered) {
            Render::VisionCapture v;
            if (Render::GetVisionCapture(v) && v.width > 0) {
                char prefix[64];
                std::snprintf(prefix, sizeof(prefix), "/vision/frame_%06llu", static_cast<unsigned long long>(frameIndex));
                const std::string base = m_directory + prefix;
                const size_t pixels = size_t(v.width) * v.height;
                Assets::Image image;
                image.width = v.width;
                image.height = v.height;
                FlipRows(v.rgba.data(), v.width, v.height, 4, image.rgba);
                bool ok = Assets::WritePng(base + "_rgb.png", image);
                std::vector<uint8_t> normals(pixels * 4), ids(pixels * 4);
                for (size_t p = 0; p < pixels; ++p) {
                    const glm::vec3 n = v.normals[p];
                    const bool background = v.entityIds[p] == 0 && glm::length(n) < 0.5f;
                    for (int c = 0; c < 3; ++c) normals[p * 4 + c] = background ? 0 : uint8_t(std::lround(std::clamp(n[c] * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f));
                    normals[p * 4 + 3] = 255;
                    const uint32_t id = v.entityIds[p];
                    for (int c = 0; c < 4; ++c) ids[p * 4 + c] = uint8_t((id >> (8 * c)) & 0xFF);
                }
                FlipRows(normals.data(), v.width, v.height, 4, image.rgba);
                ok = Assets::WritePng(base + "_normal.png", image) && ok;
                FlipRows(ids.data(), v.width, v.height, 4, image.rgba);
                ok = Assets::WritePng(base + "_id.png", image) && ok;
                ok = WritePfm(base + "_depth.pfm", v.linearDepth, v.width, v.height) && ok;
                if (ok) ++m_visionWritten;
                if (m_cameras) {
                    const float fy = v.orthographic ? v.projection[1][1] * 0.5f * float(v.height) : v.projection[1][1] * 0.5f * float(v.height);
                    const float fx = v.projection[0][0] * 0.5f * float(v.width);
                    std::fprintf(m_cameras, "%llu,%llu,%.6f,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d", static_cast<unsigned long long>(frameIndex),
                                 static_cast<unsigned long long>(totalSteps), simulationTime, v.width, v.height, fx, fy, 0.5f * v.width, 0.5f * v.height,
                                 v.nearPlane, v.farPlane, v.orthographic ? 1 : 0);
                    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) std::fprintf(m_cameras, ",%.6f", v.view[c][r]);
                    std::fputs("\n", m_cameras);
                }
                if (scene) {
                    std::vector<uint32_t> present(v.entityIds);
                    std::sort(present.begin(), present.end());
                    present.erase(std::unique(present.begin(), present.end()), present.end());
                    for (uint32_t id : present) {
                        if (id == 0 || std::any_of(m_seen.begin(), m_seen.end(), [&](const auto& s) { return s.first == id; })) continue;
                        const Entity e = scene->FromId(id - 1);
                        m_seen.emplace_back(id, e ? e.GetName() : std::string("?"));
                    }
                }
            }
        }
        if (m_csv && scene) {
            struct Row { std::string name; uint32_t id; glm::vec3 p; glm::quat q; glm::vec3 v; };
            std::vector<Row> rows;
            scene->Each<Rigidbody>([&](Entity e, Rigidbody& rb) {
                if (rb.type != Physics::BodyType::Dynamic || !e.GetBody().Valid()) return;
                rows.push_back({ e.GetName(), e.Id(), e.WorldPosition(), e.WorldRotation(), e.GetLinearVelocity() });
            });
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.name != b.name ? a.name < b.name : a.id < b.id; });
            for (const Row& r : rows) {
                std::fprintf(m_csv, "%llu,%llu,%.6f,%u,%s,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
                             static_cast<unsigned long long>(frameIndex), static_cast<unsigned long long>(totalSteps), simulationTime, r.id, r.name.c_str(),
                             r.p.x, r.p.y, r.p.z, r.q.x, r.q.y, r.q.z, r.q.w, r.v.x, r.v.y, r.v.z);
                ++m_rowsWritten;
            }
        }
    }

    void Recorder::End() {
        if (m_csv) {
            std::fclose(m_csv);
            m_csv = nullptr;
        }
        if (m_cameras) {
            std::fclose(m_cameras);
            m_cameras = nullptr;
            if (std::FILE* f = std::fopen((m_directory + "/vision/entities.csv").c_str(), "wb")) {
                std::fputs("id,name\n", f);
                std::sort(m_seen.begin(), m_seen.end());
                for (const auto& [id, name] : m_seen) std::fprintf(f, "%u,%s\n", id, name.c_str());
                std::fclose(f);
            }
            if (m_active) Log::Info() << "Recorder: " << m_visionWritten << " vision frames (rgb/depth/normal/id) written";
        }
        if (m_active) Log::Info() << "Recorder: " << m_framesWritten << " frames, " << m_rowsWritten << " data rows written";
        m_active = false;
    }
}
