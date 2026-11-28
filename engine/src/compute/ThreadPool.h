#pragma once
// Engine-private: fixed worker pool for data-parallel CPU work (CPU compute backend, CPU particles
// and XPBD of the OpenGL 4.1 tier / headless runs). ParallelFor splits [0, count) into contiguous
// chunks of at least `grain` items; the caller thread works too. Results never depend on the
// number of threads when the body writes only its own items (chunk boundaries are deterministic
// per call: chunkCount is fixed by count / grain, not by the thread count).
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Phisiks::Compute {
    class ThreadPool {
    public:
        static ThreadPool& Get();   // hardware_concurrency - 1 workers (at least 1), created on first use
        ~ThreadPool();
        size_t Workers() const { return m_threads.size(); }
        // body(chunkIndex, begin, end); returns the number of chunks used.
        size_t ParallelFor(size_t count, size_t grain, const std::function<void(size_t, size_t, size_t)>& body);
        static size_t ChunkCount(size_t count, size_t grain);

    private:
        explicit ThreadPool(size_t workers);
        void Worker();
        std::vector<std::thread> m_threads;
        std::mutex m_mutex;
        std::condition_variable m_wake, m_done;
        const std::function<void(size_t, size_t, size_t)>* m_body = nullptr;
        size_t m_count = 0, m_chunk = 0, m_chunks = 0, m_next = 0, m_finished = 0;
        uint64_t m_generation = 0;
        bool m_stop = false;
    };
}
