#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

class PS2Runtime;

// Which guest function built each GS packet, for docs/NATIVE-RENDERER.md's
// phase 0 (the census of effects).
//
// RG_DRAW_ORIGINS=FILE rewrites FILE every two seconds with one row per
// origin: what it drew per frame (primitives, sprites and their pixels at 1x),
// the targets it drew into and the textures it read, how often a texture was
// a render target (a target read back, a feedback read of the target being
// drawn, a target read in another format), and its transfers (uploads,
// readbacks, local copies). An origin is:
//  - `fn ADDR`: the call site that wrote the packet's bytes: of the outermost
//    builder (a DrawListBeginPrim to its DrawListEndPrim, DrawListAdd*,
//    DrawListClearScreen, DrawListEmitFrameEnvMaybe, GfxBlitTexRect,
//    RefractionCaptureFrame, GfxEndFrame), or else of the memory stack
//    allocation the bytes are in. Found by joining the EE address the VIF1
//    list took the packet from with the ranges of the frame's packet memory
//    each call wrote. tools/bench/origins_names.py names the call sites from
//    the function maps and sums them by function.
//  - `vu1 NAME`: what a VU1 program kicked: the name src/vu1_native.h gives
//    the program, or its tag (DQ8_GFX_PROFILE_CYCLE's) when it has none.
//  - `unattributed ADDR`: DIRECT data no hooked call wrote (by 64 KiB of EE
//    memory), `unattributed (path 3)`: the game thread's own GIF transfers.
//
// RG_DRAW_ORIGINS_FROM=N counts from the game's Nth flip (0 by default), so a
// run can leave the loading out. RG_DRAW_ORIGINS_SEQ=N writes FILE.seq: the
// order of one frame, the Nth flip's, as runs of packets of one origin into
// one target. RG_DRAW_ORIGINS_HOOK=ADDR,ADDR (hex) treats those functions as
// builders too, so a helper's callers are named instead of the helper.
namespace rg::debug
{

bool drawOriginsEnabled();

// After loadELF(): hooks the builders.
void installDrawOrigins(PS2Runtime &runtime);

// The VIF1 thread, around each list it runs: its bytes and where each piece
// came from, as (offset in the bytes, EE address); null after it.
void drawOriginsList(const uint8_t *data, size_t size, const std::vector<std::pair<uint32_t, uint32_t>> *origins);

// Any thread, as a GS packet goes to the GS: `vu1Tag` and `vu1Name` for what
// a VU1 program kicked (0 and null otherwise).
void drawOriginsPacket(const uint8_t *data, uint32_t size, uint32_t vu1Tag, const char *vu1Name);

// A native strip in place of a program's packets (RG_VU1_NATIVE=gpu).
void drawOriginsMesh(uint32_t vertices, uint32_t vu1Tag, const char *vu1Name);

} // namespace rg::debug
