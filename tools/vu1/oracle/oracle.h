#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gif_events.h"
#include "runtime/ps2_vu1.h"
#include "vu1_replay.h"

// What rg_vu1_oracle reads from a recording, and what its candidates are.
namespace rg::oracle
{

using namespace rg::gif;

struct Run
{
    rg::vu1replay::RunHeader header{};
    VU1State state{};
    std::vector<uint8_t> data;
    std::vector<std::vector<uint8_t>> kicks;
    VU1State endState{};
    bool ended = false;
    // The program it belongs to: the MSCAL's code image and entry.
    uint64_t programHash = 0u;
    uint32_t programPC = 0u;
};

struct Recording
{
    std::unordered_map<uint64_t, std::vector<uint8_t>> codes;
    std::vector<Run> runs;
};

class Candidate
{
public:
    virtual ~Candidate() = default;
    virtual const char *name() const = 0;
    // Whether it draws runs of this program (the others are not counted).
    virtual bool accepts(const Run &run) const = 0;
    // The events of `run`, every run of the file in order whether accepted or
    // not (a VU1 engine has to keep its state).
    virtual void run(const Recording &recording, const Run &run, std::vector<GifEvent> &events) = 0;
    // The registers the last run ended with, where the candidate has them.
    virtual const VU1State *endState() const { return nullptr; }
    // Lines for the report under its table, with a run count each (what it
    // did natively, what it left to the VU1 and why).
    virtual std::map<std::string, uint32_t> declined() const { return {}; }
};

// The native programs of src/vu1_native.h (native.cpp).
std::unique_ptr<Candidate> makeNative();

} // namespace rg::oracle
