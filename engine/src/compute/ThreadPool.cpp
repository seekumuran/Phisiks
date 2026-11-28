#include "compute/ThreadPool.h"

#include <algorithm>
#include <cstdint>

namespace Phisiks::Compute {
    ThreadPool& ThreadPool::Get() {
        static ThreadPool pool(std::max<size_t>(1, std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1));
        return pool;
    }

    ThreadPool::ThreadPool(size_t workers) {
        for (size_t i = 0; i < workers; ++i) m_threads.emplace_back([this] { Worker(); });
    }

    ThreadPool::~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_wake.notify_all();
        for (std::thread& t : m_threads) t.join();
    }

    size_t ThreadPool::ChunkCount(size_t count, size_t grain) {
        if (count == 0) return 0;
        grain = std::max<size_t>(grain, 1);
        return std::min<size_t>((count + grain - 1) / grain, 256);
    }

    size_t ThreadPool::ParallelFor(size_t count, size_t grain, const std::function<void(size_t, size_t, size_t)>& body) {
        const size_t chunks = ChunkCount(count, grain);
        if (chunks == 0) return 0;
        const size_t chunk = (count + chunks - 1) / chunks;
        if (chunks == 1 || m_threads.empty()) {
            for (size_t c = 0; c < chunks; ++c) body(c, c * chunk, std::min(count, (c + 1) * chunk));
            return chunks;
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        m_body = &body;
        m_count = count;
        m_chunk = chunk;
        m_chunks = chunks;
        m_next = 0;
        m_finished = 0;
        ++m_generation;
        m_wake.notify_all();
        // the caller helps
        while (m_next < m_chunks) {
            const size_t c = m_next++;
            lock.unlock();
            body(c, c * chunk, std::min(count, (c + 1) * chunk));
            lock.lock();
            ++m_finished;
        }
        m_done.wait(lock, [&] { return m_finished == m_chunks; });
        m_body = nullptr;
        return chunks;
    }

    void ThreadPool::Worker() {
        uint64_t seen = 0;
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;) {
            m_wake.wait(lock, [&] { return m_stop || (m_generation != seen && m_body && m_next < m_chunks); });
            if (m_stop) return;
            seen = m_generation;
            while (m_body && m_next < m_chunks) {
                const size_t c = m_next++;
                const auto* body = m_body;
                const size_t count = m_count, chunk = m_chunk;
                lock.unlock();
                (*body)(c, c * chunk, std::min(count, (c + 1) * chunk));
                lock.lock();
                if (++m_finished == m_chunks) m_done.notify_all();
            }
        }
    }
}
