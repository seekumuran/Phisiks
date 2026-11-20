#pragma once
// On-disk program-binary cache format (no GL dependency: unit-tested headless).
//
// File = 40-byte header + driver binary:
//   magic 'NVSC' | formatVersion u32 | key u64 | binaryFormat u32 | binarySize u32 |
//   checksum u64 (FNV-1a of the binary) | reserved u64 (0)
// The key hashes the fully preprocessed sources of every stage plus the driver identity
// (vendor/renderer/version strings), so a driver update or any source change misses the cache.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Phisiks::Render::ShaderCacheFormat {
    constexpr uint32_t MAGIC = 0x43534850u;   // "PHSC" little-endian
    constexpr uint32_t VERSION = 1;
    constexpr size_t HEADER_SIZE = 40;

    uint64_t Fnv1a64(const void* data, size_t size, uint64_t seed = 14695981039346656037ull);
    uint64_t ComputeProgramKey(const std::vector<std::string>& stageSources, const std::string& driverIdentity);
    std::string KeyToFileName(uint64_t key);   // "<16 hex digits>.phsc"

    struct ProgramBlob {
        uint64_t key = 0;
        uint32_t binaryFormat = 0;
        std::vector<uint8_t> binary;
    };

    enum class ParseResult { Ok, TooSmall, BadMagic, BadVersion, KeyMismatch, BadSize, BadChecksum, EmptyBinary };
    const char* ToString(ParseResult result);

    std::vector<uint8_t> Serialize(const ProgramBlob& blob);
    ParseResult Parse(const uint8_t* data, size_t size, uint64_t expectedKey, ProgramBlob& out);

    // Crash-safe write: the file appears either complete or not at all (temp file + atomic rename).
    bool WriteFileAtomic(const std::filesystem::path& path, const std::vector<uint8_t>& bytes);
    bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out);
}
