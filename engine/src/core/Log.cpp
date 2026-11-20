#include "Phisiks/Core/Log.h"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>

namespace Phisiks::Log {
    namespace {
        std::ofstream g_file;
        std::string g_path;
        std::mutex g_mutex;
        std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();

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
    }

    const std::string& GetLogFilePath() {
        return g_path;
    }
}
