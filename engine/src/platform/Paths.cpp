#include "platform/Paths.h"

#include <cstdlib>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <cstdint>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace Phisiks::Platform::Paths {
    namespace fs = std::filesystem;

    fs::path ExecutableDirectory() {
        std::error_code ec;
#if defined(_WIN32)
        std::vector<wchar_t> buffer(32768);
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
        if (n > 0 && n < buffer.size()) return fs::path(buffer.data()).parent_path();
#elif defined(__APPLE__)
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::vector<char> buffer(size + 1, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
            const fs::path exe = fs::weakly_canonical(fs::path(buffer.data()), ec);
            if (!ec) return exe.parent_path();
        }
#elif defined(__linux__)
        const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
        if (!ec) return exe.parent_path();
#endif
        return fs::current_path(ec);
    }

    fs::path UserCacheDirectory() {
        auto env = [](const char* name) -> std::string {
            const char* v = std::getenv(name);
            return v ? std::string(v) : std::string();
        };
#if defined(_WIN32)
        if (const std::string local = env("LOCALAPPDATA"); !local.empty()) return fs::path(local) / "Phisiks";
#elif defined(__APPLE__)
        if (const std::string home = env("HOME"); !home.empty()) return fs::path(home) / "Library" / "Caches" / "Phisiks";
#else
        if (const std::string xdg = env("XDG_CACHE_HOME"); !xdg.empty()) return fs::path(xdg) / "phisiks";
        if (const std::string home = env("HOME"); !home.empty()) return fs::path(home) / ".cache" / "phisiks";
#endif
        return {};
    }

    fs::path UserDataDirectory() {
        auto env = [](const char* name) -> std::string {
            const char* v = std::getenv(name);
            return v ? std::string(v) : std::string();
        };
#if defined(_WIN32)
        if (const std::string local = env("LOCALAPPDATA"); !local.empty()) return fs::path(local) / "Phisiks";
#elif defined(__APPLE__)
        if (const std::string home = env("HOME"); !home.empty()) return fs::path(home) / "Library" / "Application Support" / "Phisiks";
#else
        if (const std::string xdg = env("XDG_DATA_HOME"); !xdg.empty()) return fs::path(xdg) / "phisiks";
        if (const std::string home = env("HOME"); !home.empty()) return fs::path(home) / ".local" / "share" / "phisiks";
#endif
        return {};
    }
}
