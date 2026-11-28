#include "render/ShaderCacheFormat.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#define PHISIKS_GETPID _getpid
#else
#include <unistd.h>
#define PHISIKS_GETPID getpid
#endif

namespace Phisiks::Render::ShaderCacheFormat {
    namespace {
        template<class T> void Put(std::vector<uint8_t>& out, T value) {
            uint8_t bytes[sizeof(T)];
            std::memcpy(bytes, &value, sizeof(T));
            out.insert(out.end(), bytes, bytes + sizeof(T));
        }
        template<class T> T Get(const uint8_t* data, size_t offset) {
            T value;
            std::memcpy(&value, data + offset, sizeof(T));
            return value;
        }
    }

    uint64_t Fnv1a64(const void* data, size_t size, uint64_t seed) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        uint64_t hash = seed;
        for (size_t i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    uint64_t ComputeProgramKey(const std::vector<std::string>& stageSources, const std::string& driverIdentity) {
        uint64_t hash = Fnv1a64(&VERSION, sizeof(VERSION));
        hash = Fnv1a64(driverIdentity.data(), driverIdentity.size(), hash);
        for (const std::string& source : stageSources) {
            const uint64_t length = source.size();                 // length prefix: stage boundaries matter
            hash = Fnv1a64(&length, sizeof(length), hash);
            hash = Fnv1a64(source.data(), source.size(), hash);
        }
        return hash;
    }

    std::string KeyToFileName(uint64_t key) {
        char name[32];
        std::snprintf(name, sizeof(name), "%016llx.phsc", static_cast<unsigned long long>(key));
        return name;
    }

    const char* ToString(ParseResult result) {
        switch (result) {
            case ParseResult::Ok:          return "ok";
            case ParseResult::TooSmall:    return "file too small";
            case ParseResult::BadMagic:    return "bad magic";
            case ParseResult::BadVersion:  return "format version mismatch";
            case ParseResult::KeyMismatch: return "key mismatch";
            case ParseResult::BadSize:     return "size mismatch (truncated)";
            case ParseResult::BadChecksum: return "checksum mismatch (corrupt)";
            case ParseResult::EmptyBinary: return "empty binary";
        }
        return "?";
    }

    std::vector<uint8_t> Serialize(const ProgramBlob& blob) {
        std::vector<uint8_t> out;
        out.reserve(HEADER_SIZE + blob.binary.size());
        Put<uint32_t>(out, MAGIC);
        Put<uint32_t>(out, VERSION);
        Put<uint64_t>(out, blob.key);
        Put<uint32_t>(out, blob.binaryFormat);
        Put<uint32_t>(out, static_cast<uint32_t>(blob.binary.size()));
        Put<uint64_t>(out, Fnv1a64(blob.binary.data(), blob.binary.size()));
        Put<uint64_t>(out, 0);
        out.insert(out.end(), blob.binary.begin(), blob.binary.end());
        return out;
    }

    ParseResult Parse(const uint8_t* data, size_t size, uint64_t expectedKey, ProgramBlob& out) {
        if (!data || size < HEADER_SIZE) return ParseResult::TooSmall;
        if (Get<uint32_t>(data, 0) != MAGIC) return ParseResult::BadMagic;
        if (Get<uint32_t>(data, 4) != VERSION) return ParseResult::BadVersion;
        if (Get<uint64_t>(data, 8) != expectedKey) return ParseResult::KeyMismatch;
        const uint32_t binarySize = Get<uint32_t>(data, 20);
        if (binarySize == 0) return ParseResult::EmptyBinary;
        if (size != HEADER_SIZE + binarySize) return ParseResult::BadSize;
        const uint8_t* binary = data + HEADER_SIZE;
        if (Get<uint64_t>(data, 24) != Fnv1a64(binary, binarySize)) return ParseResult::BadChecksum;
        out.key = expectedKey;
        out.binaryFormat = Get<uint32_t>(data, 16);
        out.binary.assign(binary, binary + binarySize);
        return ParseResult::Ok;
    }

    bool WriteFileAtomic(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::filesystem::path temp = path;
        temp += ".tmp" + std::to_string(PHISIKS_GETPID());
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            if (!file) return false;
            file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            file.flush();
            if (!file) {
                file.close();
                std::filesystem::remove(temp, ec);
                return false;
            }
        }
        std::filesystem::rename(temp, path, ec);   // replaces atomically (MoveFileExW REPLACE_EXISTING on Windows)
        if (ec) {
            std::filesystem::remove(temp, ec);
            return false;
        }
        return true;
    }

    bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return false;
        const std::streamsize size = file.tellg();
        if (size < 0) return false;
        out.resize(size_t(size));
        file.seekg(0);
        return bool(file.read(reinterpret_cast<char*>(out.data()), size));
    }
}
