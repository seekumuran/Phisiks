#pragma once
// Game-module logging: writes to the console AND to a log file that is flushed per line,
// so logs survive crashes and forced termination (engine logs go to stdout only).

#include <sstream>
#include <string>

namespace Phisiks::Log {
    enum class Level { Info, Warning, Error };

    // Opens the log file and makes stdout unbuffered so output is not lost if the process is
    // terminated. Safe to call once at startup.
    bool Init(const std::string& logFilePath);
    void Shutdown();
    void Write(Level level, const std::string& message);
    const std::string& GetLogFilePath();

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
