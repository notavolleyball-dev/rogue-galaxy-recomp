#include "shop_prices.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps2_runtime.h"
#include "scene_timing.h"

namespace rg::shopprices
{
namespace
{

constexpr uint32_t kItemGetSellPrice = 0x003D4320u;
constexpr uint32_t kItemGetBuyPrice = 0x003D4340u;
// The item's data, and in it the price it is bought for.
constexpr uint32_t kItemData = 0x10u;
constexpr uint32_t kBuyPrice = 0x1Au;
constexpr int32_t kHighestPrice = 32767;

PS2Runtime::RecompiledFunction g_itemGetSellPrice = nullptr;
PS2Runtime::RecompiledFunction g_itemGetBuyPrice = nullptr;
std::atomic<int> g_percent{100};

int32_t scaled(int32_t price, int percent)
{
    if (price <= 0 || percent == 100)
    {
        return price;
    }
    return static_cast<int32_t>(
        std::clamp<int64_t>(static_cast<int64_t>(price) * percent / 100, 1, kHighestPrice));
}

void itemGetBuyPrice(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_itemGetBuyPrice(rdram, ctx, runtime);
    const int percent = g_percent.load(std::memory_order_relaxed);
    if (percent != 100)
    {
        setReturnS32(ctx, scaled(static_cast<int32_t>(getRegU32(ctx, 2)), percent));
    }
}

void itemGetSellPrice(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t item = getRegU32(ctx, 4) & 0x1FFFFFFFu;
    g_itemGetSellPrice(rdram, ctx, runtime);
    const int percent = g_percent.load(std::memory_order_relaxed);
    if (percent >= 100 || item + kItemData + 4u > PS2_RAM_SIZE)
    {
        return;
    }
    uint32_t data = 0u;
    std::memcpy(&data, rdram + item + kItemData, sizeof(data));
    data &= 0x1FFFFFFFu;
    if (data == 0u || data + kBuyPrice + 2u > PS2_RAM_SIZE)
    {
        return;
    }
    int16_t buy = 0;
    std::memcpy(&buy, rdram + data + kBuyPrice, sizeof(buy));
    // The price with the best card's discount (0.8), as the shop rounds it.
    const int32_t lowest = scaled(buy, percent) * 4 / 5;
    if (buy > 0 && static_cast<int32_t>(getRegU32(ctx, 2)) > lowest)
    {
        setReturnS32(ctx, lowest);
    }
}

} // namespace

void install(PS2Runtime &runtime)
{
    if (const char *value = std::getenv("RG_SHOP_PRICES"))
    {
        setPercent(std::atoi(value));
    }
    if (!rg::timing::hookFunction(runtime, kItemGetBuyPrice, &itemGetBuyPrice, g_itemGetBuyPrice) ||
        !rg::timing::hookFunction(runtime, kItemGetSellPrice, &itemGetSellPrice, g_itemGetSellPrice))
    {
        std::fprintf(stderr, "[rg] shop prices stay the game's\n");
    }
}

void setPercent(int percent)
{
    if (percent > 0)
    {
        g_percent.store(percent, std::memory_order_relaxed);
    }
}

} // namespace rg::shopprices
