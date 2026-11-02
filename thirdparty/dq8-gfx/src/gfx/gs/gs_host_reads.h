#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

// The game's reads of a render target into its own memory (Rogue Galaxy:
// added; the fade's and the menu's frame captures, sceGsExecStoreImage of
// colour), served from the GPU target in GS order without a GS transfer or
// the backend's local memory.
//
// The game thread opens a request, sends a GSNativeEffect whose command is
// kCommand | the request's id down the GS queue behind everything drawn so
// far, and waits; the backend, reaching the command, reads the target back
// and completes the request.
namespace dq8::gfx::hostreads {

constexpr uint32_t kCommand = 0x48520000u; // | id, 16 bits
constexpr uint32_t kCommandMask = 0xFFFF0000u;

struct Request {
    uint32_t base = 0;        // SBP, in blocks
    uint32_t bufferWidth = 1; // SBW, 64-pixel units
    uint32_t psm = 0;         // CT32, CT24, CT16 or CT16S
    uint32_t x = 0, y = 0, width = 0, height = 0;
};

// Set by the backend that serves them.
void setServed(bool served);
bool served();

// Game thread.
uint32_t begin(const Request &request);
// The bytes in the request's format (rows in order), or false when the
// backend could not serve it (or did not in `timeout`): read it as before.
bool wait(uint32_t id, std::vector<uint8_t> &out, std::chrono::milliseconds timeout);

// Backend.
bool pending(uint32_t id, Request &out);
void complete(uint32_t id, std::vector<uint8_t> &&bytes, bool ok);

} // namespace dq8::gfx::hostreads
