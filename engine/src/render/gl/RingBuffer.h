#pragma once
// Persistent-mapped, triple-buffered upload ring (glBufferStorage PERSISTENT | COHERENT).
// Each frame writes into its own region; a fence per region guarantees the GPU finished reading it
// before the CPU writes it again three frames later. Waits that actually block are counted.
// 4.1 tier (no persistent mapping): writes go to CPU staging memory and Flush() uploads the
// current region with glBufferSubData.

#include <glad/gl.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Phisiks::Render::GL {
    class RingBuffer {
    public:
        ~RingBuffer() { Release(); }
        void Create(const std::string& label, size_t bytesPerFrame, int frames = 3);
        void Release();

        // Starts the next region (waits for its fence if the GPU is still using it). Grows the
        // ring (re-creating it) when `bytesNeeded` exceeds the region size.
        void BeginFrame(size_t bytesNeeded);
        // Bump-allocates `bytes` (aligned) in the current region; returns the byte offset in the buffer.
        size_t Allocate(size_t bytes, size_t alignment);
        void* Pointer(size_t offset) const { return static_cast<uint8_t*>(m_mapped) + offset; }
        void Flush();                          // 4.1 tier: uploads what was written this frame (no-op on 4.6)
        void EndFrame();                       // fences the current region

        GLuint Id() const { return m_id; }
        size_t RegionSize() const { return m_regionSize; }
        uint64_t BlockedWaits() const { return m_blockedWaits; }
        uint64_t Frames() const { return m_frameCount; }

    private:
        GLuint m_id = 0;
        void* m_mapped = nullptr;
        size_t m_regionSize = 0;
        int m_regions = 3;
        int m_current = 0;
        size_t m_cursor = 0;
        GLsync m_fences[8] = {};
        uint64_t m_blockedWaits = 0;
        uint64_t m_frameCount = 0;
        std::string m_label;
        std::vector<uint8_t> m_staging;        // 4.1 tier
    };
}
