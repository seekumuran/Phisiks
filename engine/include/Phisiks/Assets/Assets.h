#pragma once
// Phisiks::Assets: file lookup and on-demand loading.
//
// Paths are relative to the asset roots: any added with AddSearchPath, then "assets/" next to the
// executable (a release package), then "assets/" in the working directory (the repository root). Images decode on a background worker
// (RequestImage / PollImage); the renderer uploads them on the main thread when ready.
// Meshes (OBJ) and audio clips (WAV) load synchronously on first use and are cached by their users.

#include "Phisiks/Render/Renderer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Phisiks::Assets {
    void AddSearchPath(const std::string& directory);         // searched before the defaults
    std::string Resolve(const std::string& path);             // existing file path, or "" if not found
    // Existing directory: absolute, or relative to the executable's folder, then the working directory.
    std::string ResolveDirectory(const std::string& path);
    bool ReadBytes(const std::string& path, std::vector<uint8_t>& out);

    struct Image {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba;   // 8-bit RGBA, top row first
    };
    bool LoadImageFile(const std::string& path, Image& out);      // synchronous (PNG, JPG, TGA, BMP)
    bool WritePng(const std::string& path, const Image& image);
    void ResizeImage(const Image& in, int width, int height, Image& out);   // bilinear

    using Ticket = uint64_t;
    enum class Status { Pending, Ready, Failed };
    Ticket RequestImage(const std::string& path);              // queued on the worker thread
    Status PollImage(Ticket ticket, Image& out);               // Ready/Failed are returned once

    bool ParseObj(const std::string& text, Render::MeshData& out, std::string* error = nullptr);
    bool LoadObj(const std::string& path, Render::MeshData& out, std::string* error = nullptr);

    struct Stats {
        uint32_t filesRead = 0;
        uint32_t imagesRequested = 0;
        uint32_t imagesDecoded = 0;
        uint32_t failures = 0;
        uint32_t pending = 0;
        uint64_t bytesRead = 0;
    };
    Stats GetStats();
    void Shutdown();                                           // stops the worker
}
