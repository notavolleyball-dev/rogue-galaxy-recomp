#pragma once

#include <cstdint>

#include "ps2_runtime.h"

namespace rg::overlay
{

// The MWo3 header fields one generated overlay tree was built from
// (config/SCUS_974.90/overlays.toml describes the format).
struct Header
{
    uint32_t id;
    uint32_t loadAddress;
    uint32_t textSize;
    uint32_t dataSize;
    uint32_t bssSize;
    uint32_t ctorBegin;
    uint32_t ctorEnd;
};

// One recompiled overlay: what it was generated from, and its function table.
struct Image
{
    const char *name;
    Header header;
    PS2Runtime::FunctionRegion *region;
};

// Defined by each overlay's generated table (src/overlay/overlay_table.cpp.in).
const Image &overlayImage_title();
const Image &overlayImage_insmenu();
const Image &overlayImage_normal();
const Image &overlayImage_factory();
const Image &overlayImage_insect();

} // namespace rg::overlay
