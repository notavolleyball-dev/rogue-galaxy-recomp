#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "runtime/gs/gs_types.h"

// VU1 programs done natively: what a program would kick for a mesh chunk,
// computed from the same VU1 data memory, for docs/NATIVE-RENDERER.md's phase 1.
//
// The game's model programs are composed per draw from pieces in six code
// slots (A at pair 0x64, B 0x12C, C 0x21C, D 0x320, E 0x4C4, F 0x73A). A
// program is recognized by the FNV-1a hash of each piece's code, so no game
// code is kept here. Every piece was read from the code with
// tools/vu1/vudis.py over a recording's coverage and is checked against the
// VU1 with tools/vu1/oracle; see its README.
//
// Done: the field's strip programs, slots A, B, C and F as one set and the
// three D pieces that finish them (no culling, the guard band, the guard band
// and backfaces), strips and triangle lists. Not done, so left to the VU1
// chunk by chunk: the backface piece's triangle lists, scrolling UVs and two
// flag branches; and slot E's clipping, except where the GPU clips (a
// GSNativeMesh drawn by SdlGpuBackend).
//
// The arithmetic per vertex is the runtime's (gsNativeMeshPoint in
// runtime/gs/gs_types.h), whichever way a chunk goes out.
namespace rg::vu1native
{

// How the D piece sets ADC on a strip's vertices.
enum class Cull : uint8_t
{
    None,
    GuardBand,
    GuardBandAndBackface,
};

struct Program
{
    const char *name;
    Cull cull;
};

// The program whose MSCAL enters at `startPC` (in bytes) with `code` (all of
// micro memory) in place, or null. Hashes about 6 KiB: callers cache it per
// code image.
const Program *identify(const uint8_t *code, uint32_t startPC);

// Why the chunk an MSCNT runs (its header at `top`, in quadwords) is left to
// the VU1, or null when it is done natively. `clips`: what draws it clips
// triangles itself (the GPU), so slot E's chunks are done too.
const char *declineChunk(const Program &program, const uint8_t *data, uint32_t top, bool clips = false);

// The GS packets the program kicks for that chunk, in order.
void buildChunk(const Program &program, const uint8_t *data, uint32_t top,
                std::vector<std::vector<uint8_t>> &packets);

// The chunk as a mesh: the packets that set the GS state it draws under
// (FOGCOL, PRIM and PRMODE), to go first, and the strip itself, for
// GS::submitNativeMesh.
void buildMesh(const Program &program, const uint8_t *data, uint32_t top,
               std::vector<std::vector<uint8_t>> &packets, GSNativeMesh &mesh);

// RG_VU1_NATIVE, in the game (src/vif1_thread.cpp), for the chunks done
// natively: `check` runs the VU1 as before and compares its packets with the
// native ones; `1` runs the VU1 (its state stays as it was) and sends the
// native packets instead of its own; `skip` sends the native packets without
// running the VU1; `gpu` sends the chunk as a mesh the renderer transforms,
// without running the VU1. Unset or 0: off.
enum class Mode : uint8_t
{
    Off,
    Check,
    Replace,
    Skip,
    Gpu,
};
Mode modeFromEnvironment();

// What the VIF1 thread did with each strip chunk, and in check mode how the
// VU1's packets compare with the native ones (within 1/16 pixel, 16 Z units,
// one color level and 1e-5 of STQ); a line every two seconds, and the first
// few differences in full.
class Monitor
{
public:
    explicit Monitor(Mode mode) : m_mode(mode) {}

    void native(const Program &program) { ++m_native[program.name]; }
    void declined(const Program &program, const char *reason);
    void check(const Program &program, const std::vector<std::vector<uint8_t>> &vu,
               const std::vector<std::vector<uint8_t>> &native);
    // Prints the line when it is due.
    void tick();

private:
    Mode m_mode;
    std::map<std::string, uint64_t> m_native;
    std::map<std::string, uint64_t> m_declined;
    uint64_t m_checked = 0u;
    uint64_t m_differ = 0u;
    uint32_t m_shown = 0u;
    int64_t m_next = 0;
};

} // namespace rg::vu1native
