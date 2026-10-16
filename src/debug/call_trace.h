#pragma once

class PS2Runtime;

namespace rg::debug
{

// RG_TRACE=0xADDR[,0xADDR...] logs the first calls of those guest functions:
// a0..a3 (as strings too, when they point at text), ra, and v0 on return.
// RG_TRACE_LIMIT=N sets how many calls per function (default 16).
// RG_TRACE_PEEK=0xADDR[,...] also prints those guest words at entry and exit.
// RG_TRACE_ARG0=0xOFF[,...] prints the words at a0+OFF at entry and exit
// (a0 as it was at entry), and the exit line adds $f0 as a float.
// RG_WATCH=0xADDR[,...] prints those guest words (I/O registers too) every
// RG_WATCH_MS milliseconds (default 1000) from a host thread.
// Call after loadELF(), like any replaceFunction() user.
void installCallTrace(PS2Runtime &runtime);

} // namespace rg::debug
