#pragma once
// Frame / simulation-data export for recorded runs (Application options exportDirectory, ...).
//   frames: <dir>/frame_000123.png (final image, top row first)
//   data:   <dir>/sim_data.csv, one row per dynamic body per exported frame:
//           frame,step,sim_time,entity,name,px,py,pz,qx,qy,qz,qw,vx,vy,vz
// Values are printed with fixed precision so identical simulations produce identical files.

#include <cstdint>
#include <cstdio>
#include <string>

namespace Phisiks {
    class Scene;

    class Recorder {
    public:
        ~Recorder() { End(); }
        bool Begin(const std::string& directory, bool frames, bool data, int every);
        // Call after the frame was simulated (and rendered, for PNG frames).
        void Capture(Scene* scene, uint64_t frameIndex, uint64_t totalSteps, double simulationTime, bool rendered);
        void End();
        bool Active() const { return m_active; }
        uint64_t FramesWritten() const { return m_framesWritten; }
        uint64_t RowsWritten() const { return m_rowsWritten; }

    private:
        std::string m_directory;
        std::FILE* m_csv = nullptr;
        bool m_active = false, m_frames = false, m_data = false, m_warnedNoRender = false;
        int m_every = 1;
        uint64_t m_framesWritten = 0, m_rowsWritten = 0;
    };
}
