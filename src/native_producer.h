#pragma once

#include <cstdint>
#include <string>

// The game side of the native pipeline (docs/NATIVE-PIPELINE.md): what turns
// the game's GS-shaped data into the renderer's records. The renderer sees
// ids only; the tables from the game's GS addresses to those ids are kept
// here, for the producers of draws (step 4). A producer may know the GS
// (this file does: the layouts the game uploads in); the renderer may not.
//
// Every function is a no-op unless the native pipeline is on
// (RG_NATIVE_PIPELINE=1 under the native renderer flag).
namespace rg::nativeproducer
{
// One mip level of an image node, as TexBuildUploadPkt sends it (game
// thread). `ram` is the level's bytes in EE memory, `ramBytes` how many
// are readable there.
struct TextureLevel
{
    const uint8_t *ram = nullptr;
    uint32_t ramBytes = 0;
    uint32_t tbp = 0;        // where the game puts it (a GS block), for the table
    uint32_t psm = 0;        // the texture's format (CT32, CT16, T8, T4)
    uint32_t width = 0;      // the texture's size in texels
    uint32_t height = 0;
    uint32_t bufferWidth = 0; // the texture's buffer width, in 64-pixel units
    // 8-bit art sent as CT32 at half the width and height, laid out so the
    // GS's memory reads it as T8: `transferWidth` by `transferHeight` CT32
    // texels at `transferBufferWidth`.
    bool preSwizzled = false;
    uint32_t transferWidth = 0;
    uint32_t transferHeight = 0;
    uint32_t transferBufferWidth = 0;
    uint32_t cbp = 0; // the node's palette (TEX0's CBP), for an indexed texture
};
void texture(const TextureLevel &level);

// A palette as the node uploads it (game thread): 256 entries (a 16x16
// image, CSM1's order) or 16 (8x2), CT32 or CT16, to the GS block `cbp`.
void palette(const uint8_t *ram, uint32_t ramBytes, uint32_t cbp, uint32_t cpsm, uint32_t entries);

// A render target the game names (gs_named_targets' kinds: 0 auto, 1 the
// frame, 2 the display buffer). Any thread.
void target(uint32_t base, uint32_t bufferWidth, uint32_t width, uint32_t height, uint32_t psm, int kind);

// The tables, for draw producers (any thread): the id of the latest texture
// at this block in this format, the palette at this block, the target at
// this base. 0 when none.
uint32_t textureAt(uint32_t tbp, uint32_t psm);
uint32_t paletteAt(uint32_t cbp);
uint32_t targetAt(uint32_t base);

// The scene target's id (the frame's).
constexpr uint32_t kSceneTarget = 1u;
} // namespace rg::nativeproducer
