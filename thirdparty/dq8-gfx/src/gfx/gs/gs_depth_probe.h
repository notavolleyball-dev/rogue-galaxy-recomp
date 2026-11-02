// Depth reads served from a GPU backend's depth, a frame late (Rogue Galaxy:
// added).
//
// A GPU backend draws depth on the GPU only, so a local-to-host read of a Z
// buffer finds in local memory nothing it drew: Rogue Galaxy's lens flare
// probes the minimum Z of an 8x8 block at the sun (GfxEndFrame_141c20, the
// slot LensFlareDraw_2ed830 arms) and, reading 0 there, is never occluded.
//
// The game thread must not wait for the GPU for it (src/depth_reads.cpp
// answers it without draining the GS worker), so the read goes through this
// mailbox: the game thread leaves the block it wants and takes the last
// block the backend copied out for it; the backend, at each frame's end,
// takes what was wanted, copies it out of the depth target and publishes it
// once the GPU is done. The answer is a frame or two old, which a lens
// flare's occlusion does not show (the game's own read is a frame late).
//
// A frame's reads are told apart by their order: the n-th read since the
// backend last took the requests is answered by the n-th block it copied
// then, if the size matches.
#pragma once

#include <cstdint>
#include <vector>

namespace dq8::gfx::depthprobe {

struct Request {
    uint32_t base = 0u;        // SBP, in 256-byte blocks
    uint32_t bufferWidth = 1u; // SBW, in units of 64 pixels
    uint32_t psm = 0u;         // the read's format (PSMZ32/24/16/16S)
    uint32_t x = 0u, y = 0u;   // SSAX, SSAY
    uint32_t width = 0u, height = 0u;
};

// The most reads one frame leaves (the game has four probe slots), and the
// largest block one may ask for, a side.
constexpr uint32_t kMaxRequests = 4u;
constexpr uint32_t kMaxSide = 64u;
constexpr uint32_t kMaxPixels = kMaxSide * kMaxSide;

// Set by the backend that serves the reads, while it can.
void setServed(bool served);
bool served();

// The game thread: the bytes for `request`, in its format (little-endian, 4,
// 3 or 2 bytes a pixel, rows in order), from the last block copied for a
// read in its place; zeros (the clear's depth, as local memory held) until
// there is one. False when the request is not one this serves.
bool read(const Request &request, std::vector<uint8_t> &out);

// The backend: what was wanted since it last took (at most kMaxRequests).
std::vector<Request> takeRequests();

// The backend: the block copied for the `index`-th request it took, as Z
// values in the depth format's range, row by row (width * height of them).
void publish(uint32_t index, const Request &request, const uint32_t *values);

// Counters, for the backend's statistics line.
struct Stats {
    uint64_t reads = 0u;     // reads answered
    uint64_t answered = 0u;  // of them, from a copied block
    uint64_t published = 0u; // blocks published
};
Stats stats();

void reset();

} // namespace dq8::gfx::depthprobe
