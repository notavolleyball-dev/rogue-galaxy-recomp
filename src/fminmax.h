#pragma once

// std::fmin / std::fmax as glibc gives them, inline. Clang lowers the
// library's to calls at the baseline x86-64 target (it cannot use MINSS: a
// NaN or a signed zero would come out differently), and in the VIF1 and
// draw threads' per-vertex loops the calls were a large share of the time.
// Same results bit for bit: a NaN gives the other operand; of +0 and -0 the
// minimum is -0 and the maximum +0, in either order (IEEE 754-2019's
// minimumNumber / maximumNumber, which glibc follows), and a signalling NaN
// gives a quiet one (x + y, as glibc returns it).

#include <cstdint>
#include <cstring>

namespace rg
{
inline bool signalling(float v)
{
    uint32_t x;
    std::memcpy(&x, &v, 4);
    return v != v && (x & 0x00400000u) == 0u;
}
inline bool signalling(double v)
{
    uint64_t x;
    std::memcpy(&x, &v, 8);
    return v != v && (x & 0x0008000000000000ull) == 0u;
}
inline float fmin(float a, float b)
{
    if (a != a || b != b)
    {
        if (signalling(a) || signalling(b))
            return a + b;
        return b != b ? a : b; // both quiet NaNs: the first
    }
    if (a < b)
        return a;
    if (b < a)
        return b;
    uint32_t x, y; // equal: only the sign of a zero can differ
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    x |= y;
    std::memcpy(&a, &x, 4);
    return a;
}
inline float fmax(float a, float b)
{
    if (a != a || b != b)
    {
        if (signalling(a) || signalling(b))
            return a + b;
        return b != b ? a : b; // both quiet NaNs: the first
    }
    if (a > b)
        return a;
    if (b > a)
        return b;
    uint32_t x, y;
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    x &= y;
    std::memcpy(&a, &x, 4);
    return a;
}
inline double fmin(double a, double b)
{
    if (a != a || b != b)
    {
        if (signalling(a) || signalling(b))
            return a + b;
        return b != b ? a : b; // both quiet NaNs: the first
    }
    if (a < b)
        return a;
    if (b < a)
        return b;
    uint64_t x, y;
    std::memcpy(&x, &a, 8);
    std::memcpy(&y, &b, 8);
    x |= y;
    std::memcpy(&a, &x, 8);
    return a;
}
inline double fmax(double a, double b)
{
    if (a != a || b != b)
    {
        if (signalling(a) || signalling(b))
            return a + b;
        return b != b ? a : b; // both quiet NaNs: the first
    }
    if (a > b)
        return a;
    if (b > a)
        return b;
    uint64_t x, y;
    std::memcpy(&x, &a, 8);
    std::memcpy(&y, &b, 8);
    x &= y;
    std::memcpy(&a, &x, 8);
    return a;
}
} // namespace rg
