#include "Phisiks/Platform/System.h"

#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <cstdio>
#include <unistd.h>
#endif

namespace Phisiks::Platform {
    uint64_t ProcessMemoryBytes() {
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS counters{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return counters.WorkingSetSize;
        return 0;
#elif defined(__APPLE__)
        mach_task_basic_info info{};
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) return info.resident_size;
        return 0;
#else
        long pages = 0, resident = 0;
        if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
            if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
            std::fclose(f);
        }
        return uint64_t(resident) * uint64_t(sysconf(_SC_PAGESIZE));
#endif
    }

    uint64_t SystemMemoryBytes() {
#if defined(_WIN32)
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        return GlobalMemoryStatusEx(&status) ? status.ullTotalPhys : 0;
#elif defined(__APPLE__)
        uint64_t bytes = 0;
        size_t size = sizeof(bytes);
        return sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0 ? bytes : 0;
#else
        return uint64_t(sysconf(_SC_PHYS_PAGES)) * uint64_t(sysconf(_SC_PAGESIZE));
#endif
    }

    unsigned HardwareThreads() { return std::thread::hardware_concurrency(); }

    const char* OperatingSystem() {
#if defined(_WIN32)
        return "Windows";
#elif defined(__APPLE__)
        return "macOS";
#else
        return "Linux";
#endif
    }
}
