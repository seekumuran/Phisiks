#include "runtime/Recorder.h"
#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/Scene.h"

#include <algorithm>
#include <filesystem>
#include <vector>

namespace Phisiks {
    bool Recorder::Begin(const std::string& directory, bool frames, bool data, int every) {
        End();
        m_directory = directory;
        m_frames = frames;
        m_data = data;
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
        m_active = true;
        Log::Info() << "Recorder: exporting" << (frames ? " PNG frames" : "") << (data ? " CSV data" : "") << " every " << m_every << " frame(s) to " << directory;
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
        if (m_active) Log::Info() << "Recorder: " << m_framesWritten << " frames, " << m_rowsWritten << " data rows written";
        m_active = false;
    }
}
