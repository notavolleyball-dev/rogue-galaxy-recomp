#pragma once

#include <cstdint>

struct VU1State;

// RG_VU1_REPLAY=<file> records every VU1 program run in full, for the oracle
// in tools/vu1/oracle: the code memory (once per distinct image), the data
// memory and registers each run starts with, the GS packets it kicks, and the
// registers it ends with. That is enough to run it again without the game and
// get the same packets, which is what a native replacement is checked against.
//
// RG_VU1_REPLAY_FROM=N starts at the N-th DMA list (default 1) and
// RG_VU1_REPLAY_LISTS=M records M lists (default 30): about 32 MB a list in
// the field, a list a frame. RG_VU1_REPLAY_AFTER=1 adds the data memory each
// run ends with, the state a following run reads. Needs the VIF1 thread
// (RG_VIF1_THREAD not 0): it runs every program through Vif1Thread::runVu1.
namespace rg::vu1replay
{

// The file. Little-endian; every record starts with its 32-bit kind.
// The file header: kMagic, kVersion, sizeof(VU1State), code size, data size.
inline constexpr uint32_t kMagic = 0x52564752u;   // "RGVR"
inline constexpr uint32_t kVersion = 1u;
// CODE: a 64-bit hash (FNV-1a over the bytes), then the code memory. Written
// before the first run that has it.
inline constexpr uint32_t kCode = 0x45444F43u;    // "CODE"
// RUN: a RunHeader, the VU1State, the data memory.
inline constexpr uint32_t kRun = 0x204E5552u;     // "RUN "
// KICK: a 32-bit size, then the GS packet the run's XGKICK sent.
inline constexpr uint32_t kKick = 0x4B43494Bu;    // "KICK"
// END: 32-bit flags, the VU1State, the data memory with kEndData.
inline constexpr uint32_t kEnd = 0x20444E45u;     // "END "

inline constexpr uint32_t kRunResume = 1u;  // MSCNT: continues the run before it
inline constexpr uint32_t kRunDBit = 2u;
inline constexpr uint32_t kRunTBit = 4u;
inline constexpr uint32_t kEndData = 1u;

struct RunHeader
{
    uint32_t flags;
    // As Vif1Thread::runVu1 had them, for VU1Interpreter::execute or resume.
    uint32_t startPC;
    uint32_t top;
    uint32_t itop;
    // The DMA list the run is part of, and the run's number in the file.
    uint32_t list;
    uint32_t index;
    // The VIF1 thread's MXCSR: rounding the compiled programs inherit.
    uint32_t mxcsr;
    uint32_t reserved;
    uint64_t codeHash;
};
static_assert(sizeof(RunHeader) == 40u);

// Whether RG_VU1_REPLAY is set; the calls below are only made when it is.
bool enabled();

// Before a run, on the thread that runs VU1, with the state it starts from.
void begin(bool resume, uint32_t startPC, uint32_t top, uint32_t itop, uint32_t list, const uint8_t *code,
           uint64_t codeGeneration, const uint8_t *data, const VU1State &state);
// A GS packet from that thread: recorded when a run is being recorded.
void kick(const uint8_t *data, uint32_t size);
// After the run.
void end(const VU1State &state, const uint8_t *data);

} // namespace rg::vu1replay
