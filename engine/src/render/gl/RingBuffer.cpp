#include "render/gl/RingBuffer.h"
#include "render/gl/GLObjects.h"
#include "Phisiks/Core/Log.h"

#include <algorithm>

namespace Phisiks::Render::GL {
    void RingBuffer::Create(const std::string& label, size_t bytesPerFrame, int frames) {
        Release();
        m_label = label;
        m_regions = std::clamp(frames, 2, 8);
        m_regionSize = (std::max<size_t>(bytesPerFrame, 4096) + 255) & ~size_t(255);
        if (Compat()) {
            m_staging.assign(m_regionSize * size_t(m_regions), 0);
            m_id = CreateBufferObject(m_regionSize * size_t(m_regions), nullptr, GL_STREAM_DRAW);
            m_mapped = m_staging.data();
            m_current = m_regions - 1;
            m_cursor = 0;
            return;
        }
        const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        glCreateBuffers(1, &m_id);
        glNamedBufferStorage(m_id, GLsizeiptr(m_regionSize * m_regions), nullptr, flags);
        m_mapped = glMapNamedBufferRange(m_id, 0, GLsizeiptr(m_regionSize * m_regions), flags);
        Label(GL_BUFFER, m_id, label);
        if (!m_mapped) Log::Error() << "RingBuffer '" << label << "': persistent mapping failed";
        m_current = m_regions - 1;
        m_cursor = 0;
    }

    void RingBuffer::Release() {
        for (GLsync& f : m_fences) {
            if (f) glDeleteSync(f);
            f = nullptr;
        }
        if (m_id) {
            if (!Compat()) glUnmapNamedBuffer(m_id);
            glDeleteBuffers(1, &m_id);
        }
        m_id = 0;
        m_mapped = nullptr;
        m_staging.clear();
    }

    void RingBuffer::BeginFrame(size_t bytesNeeded) {
        if (!m_id || bytesNeeded > m_regionSize) {
            if (m_id) glFinish();   // growing: make sure nothing still reads the old buffer
            Create(m_label, std::max(bytesNeeded + bytesNeeded / 2, m_regionSize * 2), m_regions);
        }
        m_current = (m_current + 1) % m_regions;
        m_cursor = 0;
        GLsync& fence = m_fences[m_current];
        if (fence) {
            GLenum status = glClientWaitSync(fence, 0, 0);
            if (status == GL_TIMEOUT_EXPIRED) {
                ++m_blockedWaits;
                status = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
            }
            glDeleteSync(fence);
            fence = nullptr;
        }
        ++m_frameCount;
    }

    size_t RingBuffer::Allocate(size_t bytes, size_t alignment) {
        alignment = std::max<size_t>(alignment, 16);
        m_cursor = (m_cursor + alignment - 1) / alignment * alignment;
        const size_t offset = size_t(m_current) * m_regionSize + m_cursor;
        m_cursor += bytes;
        if (m_cursor > m_regionSize) Log::Error() << "RingBuffer '" << m_label << "': region overflow (" << m_cursor << " > " << m_regionSize << ")";
        return offset;
    }

    void RingBuffer::Flush() {
        if (!Compat() || !m_id || m_cursor == 0) return;
        const size_t start = size_t(m_current) * m_regionSize;
        BufferSubData(m_id, start, std::min(m_cursor, m_regionSize), m_staging.data() + start);
    }

    void RingBuffer::EndFrame() {
        if (!m_id || Compat()) return;
        if (m_fences[m_current]) glDeleteSync(m_fences[m_current]);
        m_fences[m_current] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
}
