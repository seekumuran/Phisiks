#pragma once
// Engine-private: per-OS file-system locations (std::filesystem everywhere else).
#include <filesystem>

namespace Phisiks::Platform::Paths {
    std::filesystem::path ExecutableDirectory();   // falls back to the working directory
    // Per-user cache root: %LOCALAPPDATA%\Phisiks (Windows), ~/Library/Caches/Phisiks (macOS),
    // $XDG_CACHE_HOME/phisiks or ~/.cache/phisiks (Linux). Empty if none can be determined.
    std::filesystem::path UserCacheDirectory();
}
