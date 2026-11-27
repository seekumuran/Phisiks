#include "Phisiks/Core/Log.h"

#include <chrono>
#include <deque>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <mutex>

namespace Phisiks::Log {
    namespace {
        std::ofstream g_file;
        std::string g_path;
        std::mutex g_mutex;
        std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();
        std::deque<Record> g_recent;
        uint64_t g_lines = 0;
        constexpr size_t RECENT = 2000;

        const char* LevelTag(Level level) {
            switch (level) {
                case Level::Info:    return "INFO ";
                case Level::Warning: return "WARN ";
                case Level::Error:   return "ERROR";
            }
            return "?????";
        }
    }

    bool Init(const std::string& logFilePath) {
        // Unbuffered stdout: engine logging uses std::cout, which is fully buffered when redirected.
        if (!std::getenv("HELL_NO_SETVBUF")) std::setvbuf(stdout, nullptr, _IONBF, 0);

        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(logFilePath).parent_path(), ec);
        g_file.open(logFilePath, std::ios::out | std::ios::trunc);
        g_path = logFilePath;
        g_start = std::chrono::steady_clock::now();
        if (!g_file) {
            std::cerr << "[Game] could not open log file " << logFilePath << "\n";
            return false;
        }
        return true;
    }

    void Shutdown() {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_file.is_open()) g_file.close();
    }

    void Write(Level level, const std::string& message) {
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
        char prefix[48];
        std::snprintf(prefix, sizeof(prefix), "[%9.3f] [PHISIKS %s] ", seconds, LevelTag(level));

        std::lock_guard<std::mutex> lock(g_mutex);
        std::cout << prefix << message << "\n";
        if (g_file.is_open()) {
            g_file << prefix << message << "\n";
            g_file.flush();
        }
        g_recent.push_back(Record{ level, seconds, message, ++g_lines });
        if (g_recent.size() > RECENT) g_recent.pop_front();
    }

    void CopyRecent(std::vector<Record>& out, size_t maxLines) {
        std::lock_guard<std::mutex> lock(g_mutex);
        const size_t n = std::min(maxLines, g_recent.size());
        out.assign(g_recent.end() - std::ptrdiff_t(n), g_recent.end());
    }

    uint64_t LineCount() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_lines;
    }

    const std::string& GetLogFilePath() {
        return g_path;
    }
}
