#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "gfx/native/native_records.h"
#include "runtime/gs/gs_types.h"

// M5 step 3 of docs/NATIVE-ONLY.md: model draws made on the game thread.
// The game thread's gather (src/vif1_thread.cpp's gatherChain) walks each
// list in DMA order as it copies it; a StreamReader reads the same bytes as
// the VIF would (UNPACK, MPG, the base program at MSCAL 0, DIRECT's GS
// writes), into a VU1 data and code memory and a draw state of its own. A
// model chunk the list no longer carries (a ModelMark) is drawn from those:
// its header, the program's constants, its bones, its state packets, made
// here, so the VIF1 thread has nothing to compute for it.
// RG_MODEL_GAME_TIME=0: the VIF1 thread makes them from its VU1 memory.
namespace rg::nativemodels
{

bool enabled();

// A model chunk's draw, made on the game thread.
struct GameDraw
{
    uint32_t id = 0u;
    GSNativeMesh mesh; // its header, no vertices
    std::vector<std::vector<uint8_t>> packets; // FOGCOL, then PRIM and PRMODE
    dq8::gfx::native::ModelRow row{};
    std::vector<float> palette;
    std::array<uint8_t, 64u * 16u> vu{}; // VU1 quads 0-63, as the producer reads them
};

class StreamReader
{
public:
    StreamReader();
    ~StreamReader();
    StreamReader(const StreamReader &) = delete;
    StreamReader &operator=(const StreamReader &) = delete;
    // The list's bytes, in the order the VIF reads them.
    void read(const uint8_t *data, uint32_t size);
    // Bytes the copy left out (an upload's data the producer holds).
    void skip(uint32_t size);
    // A chunk the list does not carry, where its MSCNT is: its draw (null
    // when the program loaded is not one the model stage draws), then the
    // MSCNT's buffer swap.
    std::shared_ptr<GameDraw> chunk(const uint8_t header[32], uint32_t slots, uint32_t id);

private:
    struct Impl;
    Impl *m_impl;
};

// The game thread's.
StreamReader &reader();

} // namespace rg::nativemodels
