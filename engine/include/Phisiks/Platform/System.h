#pragma once
// Process / machine information for telemetry (Windows, Linux, macOS).
#include <cstdint>
#include <string>

namespace Phisiks::Platform {
    uint64_t ProcessMemoryBytes();    // resident set / working set (0 if unknown)
    uint64_t SystemMemoryBytes();     // installed physical memory (0 if unknown)
    unsigned HardwareThreads();
    const char* OperatingSystem();    // "Windows", "Linux", "macOS"
}
