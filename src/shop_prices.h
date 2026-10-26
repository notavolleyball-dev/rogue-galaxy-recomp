#pragma once

class PS2Runtime;

// What shops ask for their items.
//
// A shop's row takes the item's price from ItemGetBuyPrice (0x3D4340, the
// half word at +0x1A of the item's data): ShopRowGetPrice (0x3CE6E0) for the
// price the shop shows and ShopBuyItem charges, with a member's card's
// discount (0.8 or 0.9), and ShopRowGetBasePrice (0x3CE7F0) for the one
// before it. The menu's game number `shop_prices`, a percentage, scales
// that price, at least 1 and at most the game's 32767. What an item sells
// for (ItemGetSellPrice, 0x3D4320, +0x1C) stays the game's, but below 100%
// never more than the item now costs with the best card, so a cheap shop
// cannot be sold back to for a profit.
namespace rg::shopprices
{

// After loadELF().
void install(PS2Runtime &runtime);

// Percent of the game's.
void setPercent(int percent);

} // namespace rg::shopprices
