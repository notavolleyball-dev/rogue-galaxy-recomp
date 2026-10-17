#pragma once

class PS2Runtime;

namespace rg::debug
{

// RG_HWWATCH=0xADDR watches the guest word at ADDR (4-aligned) with an x86
// debug register on every thread of the process, so a write from guest code,
// the runtime or a host thread traps right after it happens. A write that
// leaves the word inside RG_HWWATCH_RANGE=0xLO-0xHI (default: the main
// stack, 0x01FD0000-0x02000000) prints the writer's thread and call stack to
// stderr. With RG_HWWATCH_READER=0xFUNC it watches reads too and reports
// instead when that guest function reads back a value in range, with the
// last eight writes to the slot and their writers' call stacks.
// RG_HWWATCH_DELAY=N arms it only N seconds after start. Windows only. Call
// after loadELF().
void installHardwareWatch(PS2Runtime &runtime);

} // namespace rg::debug
