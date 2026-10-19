#include "stick_response.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace rg::pad
{
namespace
{

// The game reads each stick axis as (byte - 128), drops the first 50 of
// its 128 steps and scales the rest to 0..1 (PadGetAnalogLXNorm, 0x183AF0),
// and the player control (0x1FC4A0) only runs when the squared length of
// the pair reaches 0.81. A DualShock 2 fills its square range, so on the
// console a full diagonal reads close to (1, 1) and the party runs any way
// the stick points. The sticks SDL reads (Xbox, DualShock 4, DualSense) are
// round: a full diagonal is (0.7, 0.7), which the game's deadzone turns
// into (0.5, 0.5), length 0.74, a walk. The party ran only within about 20
// degrees of each of the four directions and slowed to a walk in between.
//
// This keeps the direction and the distance the stick was pushed, but
// measures that distance the way the console's stick reports it, as the
// longer of the two axes: a full diagonal becomes (1, 1), a half push stays
// a half push, the four straight directions do not change.
bool enabled()
{
    static const bool value = [] {
        const char *env = std::getenv("RG_STICK_SQUARE");
        return !(env && *env == '0');
    }();
    return value;
}

float axisValue(uint8_t byte)
{
    return (static_cast<float>(byte) - 128.0f) / 128.0f;
}

uint8_t axisByte(float value)
{
    const float scaled = std::round(128.0f + value * 128.0f);
    return static_cast<uint8_t>(std::clamp(scaled, 0.0f, 255.0f));
}

void square(uint8_t &xByte, uint8_t &yByte)
{
    const float x = axisValue(xByte);
    const float y = axisValue(yByte);
    const float longest = std::max(std::fabs(x), std::fabs(y));
    if (longest <= 0.0f)
    {
        return;
    }
    const float travel = std::min(1.0f, std::sqrt(x * x + y * y));
    const float scale = travel / longest;
    xByte = axisByte(std::clamp(x * scale, -1.0f, 1.0f));
    yByte = axisByte(std::clamp(y * scale, -1.0f, 1.0f));
}

} // namespace

uint32_t shapeSticks(uint32_t sticks)
{
    if (!enabled())
    {
        return sticks;
    }
    uint8_t rx = static_cast<uint8_t>(sticks >> 24);
    uint8_t ry = static_cast<uint8_t>(sticks >> 16);
    uint8_t lx = static_cast<uint8_t>(sticks >> 8);
    uint8_t ly = static_cast<uint8_t>(sticks);
    square(lx, ly);
    square(rx, ry);
    return (static_cast<uint32_t>(rx) << 24) | (static_cast<uint32_t>(ry) << 16) |
           (static_cast<uint32_t>(lx) << 8) | static_cast<uint32_t>(ly);
}

} // namespace rg::pad
