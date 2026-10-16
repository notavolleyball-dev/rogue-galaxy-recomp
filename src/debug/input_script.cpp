#include "debug/input_script.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Stubs/Pad.h"

namespace rg::debug
{
namespace
{

struct Step
{
    double seconds;
    uint16_t mask; // pressed buttons, active-high here
    unsigned ms;
    uint8_t lx = 0x80; // left stick, 0x80 = centred
    uint8_t ly = 0x80;
};

// Bit order of the PS2 pad's button word (active-low on the wire).
constexpr const char *kButtons[16] = {
    "SELECT", "L3", "R3", "START", "UP", "RIGHT", "DOWN", "LEFT",
    "L2", "R2", "L1", "R1", "TRIANGLE", "CIRCLE", "CROSS", "SQUARE",
};

bool parseButtons(const std::string &text, Step &step)
{
    uint16_t &mask = step.mask;
    std::stringstream ss(text);
    std::string name;
    bool any = false;
    while (std::getline(ss, name, '+'))
    {
        // Left stick pushed all the way: LUP LDOWN LLEFT LRIGHT.
        if (name == "LUP" || name == "LDOWN" || name == "LLEFT" || name == "LRIGHT")
        {
            (name == "LUP" || name == "LDOWN" ? step.ly : step.lx) =
                (name == "LUP" || name == "LLEFT") ? 0x00 : 0xFF;
            any = true;
            continue;
        }
        const auto it = std::find_if(std::begin(kButtons), std::end(kButtons),
                                     [&](const char *b) { return name == b; });
        if (it == std::end(kButtons))
        {
            return false;
        }
        mask |= static_cast<uint16_t>(1u << (it - std::begin(kButtons)));
        any = true;
    }
    return any;
}

std::vector<Step> parse(const char *script)
{
    std::vector<Step> steps;
    std::stringstream ss(script);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        const size_t a = item.find(':');
        const size_t b = item.find(':', a + 1);
        Step step{};
        if (a == std::string::npos || b == std::string::npos ||
            !parseButtons(item.substr(a + 1, b - a - 1), step))
        {
            std::fprintf(stderr, "[input] cannot parse '%s'\n", item.c_str());
            continue;
        }
        step.seconds = std::atof(item.substr(0, a).c_str());
        step.ms = static_cast<unsigned>(std::strtoul(item.substr(b + 1).c_str(), nullptr, 10));
        steps.push_back(step);
    }
    std::sort(steps.begin(), steps.end(), [](const Step &l, const Step &r) { return l.seconds < r.seconds; });
    return steps;
}

} // namespace

void startInputScript()
{
    const char *script = std::getenv("RG_INPUT");
    if (!script || !*script)
    {
        return;
    }
    std::vector<Step> steps = parse(script);
    const auto start = std::chrono::steady_clock::now();
    std::thread([steps, start]() {
        constexpr uint8_t kCenter = 0x80;
        for (const Step &step : steps)
        {
            std::this_thread::sleep_until(start + std::chrono::duration<double>(step.seconds));
            std::printf("[input] %.1fs: buttons 0x%04X for %u ms\n", step.seconds, step.mask, step.ms);
            std::fflush(stdout);
            ps2_stubs::setPadOverrideState(static_cast<uint16_t>(~step.mask), step.lx, step.ly, kCenter, kCenter);
            std::this_thread::sleep_for(std::chrono::milliseconds(step.ms));
            ps2_stubs::setPadOverrideState(0xFFFFu, kCenter, kCenter, kCenter, kCenter);
        }
        ps2_stubs::clearPadOverrideState();
    }).detach();
}

} // namespace rg::debug
