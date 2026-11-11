#include "gfx/backends/sdlgpu/sdlgpu_window.h"

#include <algorithm>
#include <cmath>

namespace dq8::gfx {

double sdlGpuWideAspect(uint32_t windowWidth, uint32_t windowHeight) {
    const double window = double(std::max(windowWidth, 1u)) / double(std::max(windowHeight, 1u));
    return std::clamp(window, 4.0 / 3.0, 32.0 / 9.0);
}

double sdlGpuDisplayAspect(const SdlGpuDisplayOptions &options, uint32_t sourceWidth,
                           uint32_t sourceHeight, uint32_t windowWidth, uint32_t windowHeight,
                           int gameWidescreen) {
    switch (options.aspect) {
    case SdlGpuAspect::Auto:
        return gameWidescreen == 1 ? sdlGpuWideAspect(windowWidth, windowHeight) : 4.0 / 3.0;
    case SdlGpuAspect::Standard:
        return 4.0 / 3.0;
    case SdlGpuAspect::Wide:
        return 16.0 / 9.0;
    case SdlGpuAspect::Native:
        return double(std::max(sourceWidth, 1u)) / double(std::max(sourceHeight, 1u));
    case SdlGpuAspect::Stretch:
        break;
    }
    return double(std::max(windowWidth, 1u)) / double(std::max(windowHeight, 1u));
}

SdlGpuRect sdlGpuDisplayRect(const SdlGpuDisplayOptions &options, uint32_t sourceWidth,
                             uint32_t sourceHeight, uint32_t windowWidth, uint32_t windowHeight,
                             int gameWidescreen) {
    if (sourceWidth == 0u || sourceHeight == 0u || windowWidth == 0u || windowHeight == 0u)
        return {};
    const double aspect = sdlGpuDisplayAspect(options, sourceWidth, sourceHeight, windowWidth,
                                              windowHeight, gameWidescreen);
    double width = windowWidth;
    double height = width / aspect;
    if (height > windowHeight) {
        height = windowHeight;
        width = height * aspect;
    }
    if (options.integerScale && options.aspect != SdlGpuAspect::Stretch) {
        // The largest whole multiple of the source height whose width at this
        // aspect still fits; square pixels then come out exact on both axes.
        for (uint32_t factor = static_cast<uint32_t>(windowHeight / sourceHeight); factor >= 1u; --factor) {
            const double scaledHeight = double(sourceHeight) * factor;
            const double scaledWidth = options.aspect == SdlGpuAspect::Native
                                           ? double(sourceWidth) * factor
                                           : scaledHeight * aspect;
            if (scaledWidth <= windowWidth + 0.5) {
                width = scaledWidth;
                height = scaledHeight;
                break;
            }
        }
    }
    SdlGpuRect rect;
    rect.width = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(width)), 1u, windowWidth);
    rect.height = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(height)), 1u, windowHeight);
    rect.x = (windowWidth - rect.width) / 2u;
    rect.y = (windowHeight - rect.height) / 2u;
    return rect;
}

} // namespace dq8::gfx
