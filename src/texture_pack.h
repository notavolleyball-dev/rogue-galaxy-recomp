#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The game's art at a higher resolution: replacement packs kept on the
// player's machine (made from their own disc or by others, never in the
// repository); docs/TEXTURE_PACKS.md.
//
// Reading (textures\hd, then textures\packs by name; RG_TEXTURE_PACK=DIR for
// one pack alone): a pack made for PCSX2 (TEX0HASH-CLUTHASH-BITS.png or
// .dds) replaces whole textures, the scenery's and the characters', at any
// scale. The native producer asks (whole) when it binds an indexed texture
// to a palette, by the hashes PCSX2 names them by (src/pcsx2_hash.h), and
// the renderer draws the picture in its place.
//
// The packs of 2D art by region (TEXELS_CLUT_WxH.png, collected with
// RG_TEXTURE_DUMP: glyphs, labels, icons) are read and listed, but not drawn
// until M4 of docs/NATIVE-ONLY.md gives the 2D its own records: the GS
// backend that drew them is gone (decided 2026-11-11).
namespace rg::texturePack
{

// Before the game runs: reads the packs and starts the thread that decodes
// their pictures.
void install();

// The packs found, in the order they are read (a later one over an earlier
// one): `key` names the pack's switch in the settings (texture_pack_KEY),
// `enabled` is where it starts.
struct PackInfo
{
    std::string key, name;
    size_t regions;
    bool enabled;
    // Of the pictures, a PCSX2 pack's whole textures, drawn at any scale;
    // the rest is 2D art by region.
    size_t wholes;
};
std::vector<PackInfo> packs();

// A pack switched: the answers so far no longer hold (generation()).
void setPackEnabled(const std::string &key, bool on);

// Block formats a picture may stay in (or'd), when the GPU samples them.
constexpr int kBc1 = 1;
constexpr int kBc2 = 2;
constexpr int kBc3 = 4;
constexpr int kBc7 = 8;

struct Picture
{
    uint32_t width = 0, height = 0;
    // RGBA8 with the game's alpha (0x80 opaque), or with `blocks` (a kBc*)
    // the picture's 4x4 blocks as the file has them, a row of them after
    // another.
    std::vector<uint8_t> data;
    int blocks = 0;
    // Drawn from its own texels, not filtered across them (pack.json's
    // "filter": "nearest").
    bool nearest = false;
};

enum class Lookup
{
    None,    // no picture for it; not asked again until the packs change
    Pending, // being read: drawn as it is meanwhile, asked again later
    Ready,   // in `out`, once: the caller keeps it
};

// The picture of a whole texture `width` by `height` whose texels and
// palette hash to `texels` and `clut` as PCSX2 hashes them; `blockFormats`
// are the kBc* formats the caller can take as they are. Any thread.
Lookup whole(uint64_t texels, uint64_t clut, uint32_t width, uint32_t height, int blockFormats, Picture &out);

// Changes whenever the packs switched: what was answered before is stale.
// Any thread, without a lock.
uint64_t generation();

// Whether any enabled pack has a picture at all. Any thread, without a lock.
bool active();

} // namespace rg::texturePack
