#pragma once

#include <string>

class PS2Runtime;

// Three more pages in the game's own System > Settings, turned with L1 and
// R1: the F1 menu's switches (On / Off), its numbers (Low takes a step off,
// High adds one) and its quick scenes (On / Off), each change kept in
// settings.ini as the F1 menu's own are.
//
// The screen is the main menu's (its widgets in a list from the pointer at
// 0x482360, found by id): ten row names, the text widget 0x1B4's ten strings;
// two choices a row, the sprite widgets 0x1B7 + 2 x row and the one after,
// each a button (one picture for all, "Sys_01a") and a word (Low, High, On,
// Off, Stereo, ..., a piece of the same picture), the picked one white and
// the other 0.6 grey with its button's v on the picture's lower half
// (MenuOptionsChoiceHighlight, 0x34FA80); the row's bar 0x1B5 and the hand
// 0x1CB; and the cursor's grid,
// the entry with tag 0x1D among those at +4 of the list at 0x482364 (+0xE its
// last row, +0x14 and +0x16 the column and row). States 0x7F to 0x81 open it,
// read the cursor (MenuOptionsSelectState, 0x34FE70) and close it.
//
// Turning a page writes those fields: our strings (in memory of the
// runtime's) for the names, the On and Off words (row 3's pieces) or the Low
// and High ones (row 0's) for the choices, the rows past the page's hidden and
// out of the cursor's reach. While a page of ours is up, the screen's own
// functions that would read or set the game's settings (MenuOptionsSetItem,
// MenuOptionLoadValue, MenuOptionsRefresh) act on ours instead, and the
// game's page comes back as it was when the screen opens or closes.
namespace rg::settingspages
{

// After loadELF().
void install(PS2Runtime &runtime);

// What the F1 menu set, for the pages to show (any thread).
void noteOption(const std::string &key, bool on);
void noteNumber(const std::string &key, int value);
// A number's range, step and default, from the F1 menu's option.
void setRange(const std::string &key, int minimum, int maximum, int step, int defaultValue);

// A change made on a page, for the main thread to hand the F1 menu, which
// keeps and applies it; false when none is waiting.
struct Change
{
    std::string key;
    bool number = false;
    int value = 0;
};
bool takeChange(Change &change);

} // namespace rg::settingspages
