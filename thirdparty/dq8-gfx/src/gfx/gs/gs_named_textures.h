#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Textures the game names as it uploads them (Rogue Galaxy: added).
//
// The game builds every texture upload from its name table's image node
// (TexBuildUploadPkt_158610 in RGDecomp: the node's size, depth, mip
// addresses and palette). The host hooks it and queues, per transfer the
// packets will make, what the transfer really is: an image of which format
// and size, or a palette. The backend matches each host-to-GS transfer with
// the head of the queue, in GS order, and keeps the transfer's bytes as that
// image, so a texture is built from the image as the game made it rather
// than read back through the GS's memory layout.
//
// The queue is filled when the packets are built and drained as the GS
// consumes them; both happen in the draw list's order.
namespace dq8::gfx::namedtextures {

struct Upload {
    // The transfer as the GS sees it (BITBLTBUF and TRXREG; TRXPOS 0,0).
    uint32_t dbp = 0;
    uint32_t dbw = 0;
    uint32_t dpsm = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    // What it is: the texture's own format and buffer width, and its size in
    // texels. They differ from the transfer's for 8-bit art the game sends
    // pre-swizzled as CT32 at half the width and height (`preSwizzled`).
    uint32_t psm = 0;
    uint32_t textureBufferWidth = 0;
    uint32_t textureWidth = 0;
    uint32_t textureHeight = 0;
    bool preSwizzled = false;
    bool palette = false;
    // The game's name for it: the image node's address.
    uint32_t node = 0;
};

// Game thread.
void push(const Upload &upload);

// Backend: the queued record this transfer is, if one of the next few
// matches it (records before the match were never sent and are dropped).
bool take(uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t width, uint32_t height, Upload &out);

// Whether one of the next few queued records matches, without taking it.
bool peek(uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t width, uint32_t height);

bool active();
void reset();

// Uploads held before the GS queue (Rogue Galaxy: added; the host's
// held_uploads): a named upload's bytes, whole, and their hash, handed to
// the backend beside the transfer rather than through the queue. `transfer`
// counts the transfers the backend is sent (from 1), so the backend knows
// which of its transfers is held: no payload follows that one.
struct Held {
    uint64_t transfer = 0;
    uint64_t hash = 0; // imageHash of `bytes`
    std::shared_ptr<std::vector<uint8_t>> bytes;
};
// The feeding thread, just before it sends the held transfer on.
void hold(Held held);
// The backend, at each transfer it is sent, numbered as the feeding thread
// numbers them: the held upload it is, if it is one.
bool takeHeld(uint64_t transfer, Held &out);
bool anyHeld();

// The identity of an image's bytes, never 0. Shared by the feeding thread
// and the backend, so an image hashed on either side is the same image.
uint64_t imageHash(const uint8_t *data, size_t size);

struct Stats {
    uint64_t pushed = 0;
    uint64_t matched = 0;
    uint64_t dropped = 0;
};
Stats stats();

} // namespace dq8::gfx::namedtextures
