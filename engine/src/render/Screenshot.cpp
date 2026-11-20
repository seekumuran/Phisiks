#include "Phisiks/Render/RenderStats.h"
#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Core/Log.h"
#include "render/RendererInternal.h"

#include <algorithm>
#include <vector>

namespace Phisiks {
    // Writes the image the renderer last presented (scene + overlay text). Reading the renderer's
    // own texture, not the window's back buffer, makes screenshots independent of swap-chain
    // behaviour (buffer age, flip model).
    bool CaptureScreenshot(const std::string& path) {
        std::vector<uint8_t> rgba;
        int width = 0, height = 0;
        if (!Render::Internal::ReadPresented(rgba, width, height)) return false;
        Assets::Image image;
        image.width = width;
        image.height = height;
        image.rgba.resize(rgba.size());
        const size_t row = size_t(width) * 4;
        for (int y = 0; y < height; ++y) {   // GL rows are bottom-up; PNG rows top-down
            std::copy_n(rgba.data() + size_t(height - 1 - y) * row, row, image.rgba.data() + size_t(y) * row);
        }
        const bool ok = Assets::WritePng(path, image);
        if (ok) Log::Info() << "Screenshot " << path << " (" << width << "x" << height << ")";
        return ok;
    }
}
