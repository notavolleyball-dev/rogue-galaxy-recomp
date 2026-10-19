#pragma once

#include <cstdio>

namespace rg::debug
{

// Prints every host thread of the process with its stack, one line each:
// the thread's name and id, then the frames innermost first, each as
// module+offset, or for the executable as its link-time address
// (exe!0x14...), which llvm-symbolizer resolves against the built exe:
//   grep -o 'exe!0x[0-9a-f]*' build/last-run.log | sed 's/exe!//' | sort -u \
//     | llvm-symbolizer --obj=build/game/src/roguegalaxy.exe
// Suspends each thread for the walk (microseconds) and allocates nothing
// while one is suspended. For the stall watch: where the host is when the
// guest shows nothing. Windows only; elsewhere it prints nothing.
void printHostStacks(std::FILE *out, const char *prefix);

} // namespace rg::debug
