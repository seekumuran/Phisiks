#pragma once
// Game-module logging: writes to the console AND to a log file that is flushed per line,
// so logs survive crashes and forced termination (engine logs go to stdout only).

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace Phisiks::Log {
    enum class Level { Info, Warning, Error };

    // Opens the log file and makes stdout unbuffered so output is not lost if the process is
    // terminated. Safe to call once at startup.
    bool Init(const std::string& logFilePath);
    void Shutdown();
    void Write(Level level, const std::string& message);
    const std::string& GetLogFilePath();

    // The most recent lines (any thread), oldest first, for in-app consoles. The engine keeps the
    // last 2000; `sequence` grows by one per line ever written (detects new lines cheaply).
    struct Record { Level level = Level::Info; double seconds = 0.0; std::string text; uint64_t sequence = 0; };
    void CopyRecent(std::vector<Record>& out, size_t maxLines = 2000);
    uint64_t LineCount();

    struct Line {
        explicit Line(Level level) : m_level(level) {}
        ~Line() { Write(m_level, m_stream.str()); }
        template<class T> Line& operator<<(const T& value) { m_stream << value; return *this; }
    private:
        Level m_level;
        std::ostringstream m_stream;
    };

    inline Line Info()    { return Line(Level::Info); }
    inline Line Warning() { return Line(Level::Warning); }
    inline Line Error()   { return Line(Level::Error); }
}
