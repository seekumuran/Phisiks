#pragma once
// Process / machine information for telemetry (Windows, Linux, macOS).
#include <cstdint>
#include <string>

namespace Phisiks::Platform {
    uint64_t ProcessMemoryBytes();    // resident set / working set (0 if unknown)
    uint64_t SystemMemoryBytes();     // installed physical memory (0 if unknown)
    unsigned HardwareThreads();
    const char* OperatingSystem();    // "Windows", "Linux", "macOS"

    // Folder of the running executable (falls back to the working directory).
    std::string ExecutableDirectory();
    // Per-user writable data folder: %LOCALAPPDATA%\Phisiks (Windows), ~/Library/Application Support/Phisiks
    // (macOS), $XDG_DATA_HOME/phisiks or ~/.local/share/phisiks (Linux). Empty if none can be determined.
    std::string UserDataDirectory();
    // True if `directory` exists (or can be created) and a file can be written in it.
    bool IsWritableDirectory(const std::string& directory);
}
