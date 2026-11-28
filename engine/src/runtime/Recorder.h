#pragma once
// Frame / simulation-data export for recorded runs (Application options exportDirectory, ...).
//   frames: <dir>/frame_000123.png (final image, top row first)
//   data:   <dir>/sim_data.csv, one row per dynamic body per exported frame:
//           frame,step,sim_time,entity,name,px,py,pz,qx,qy,qz,qw,vx,vy,vz
//   vision: <dir>/vision/frame_000123_{rgb,normal,id}.png + _depth.pfm (aligned, scene resolution):
//           rgb tonemapped; normal = world normal * 0.5 + 0.5 (black = background); id = Entity::Id()+1
//           as little-endian RGBA bytes (0 = background); depth = linear metres along the view axis
//           (PFM, float32, 0 = background). vision/cameras.csv: per frame intrinsics + view matrix;
//           vision/entities.csv: id,name of every entity seen.
// Values are printed with fixed precision so identical simulations produce identical files.

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace Phisiks {
    class Scene;

    class Recorder {
    public:
        ~Recorder() { End(); }
        bool Begin(const std::string& directory, bool frames, bool data, int every, bool vision = false);
        bool WantsVision(uint64_t frameIndex) const { return m_active && m_vision && frameIndex % uint64_t(m_every) == 0; }
        uint64_t VisionFramesWritten() const { return m_visionWritten; }
        // Call after the frame was simulated (and rendered, for PNG frames).
        void Capture(Scene* scene, uint64_t frameIndex, uint64_t totalSteps, double simulationTime, bool rendered);
        void End();
        bool Active() const { return m_active; }
        uint64_t FramesWritten() const { return m_framesWritten; }
        uint64_t RowsWritten() const { return m_rowsWritten; }

    private:
        std::string m_directory;
        std::FILE* m_csv = nullptr;
        std::FILE* m_cameras = nullptr;
        bool m_active = false, m_frames = false, m_data = false, m_vision = false, m_warnedNoRender = false;
        uint64_t m_visionWritten = 0;
        std::vector<std::pair<uint32_t, std::string>> m_seen;
        int m_every = 1;
        uint64_t m_framesWritten = 0, m_rowsWritten = 0;
    };
}
