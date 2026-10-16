#pragma once

namespace rg::debug
{

// RG_SELFTEST=gs runs checks of code we changed in vendored dependencies
// instead of the game, and exits with their result. Returns false (and runs
// nothing) when the variable is unset.
bool runSelfTestIfRequested(int &status);

} // namespace rg::debug
