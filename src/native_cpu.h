#pragma once

class PS2Runtime;

// The native CPU (docs/NATIVE-CPU-GOAL.md, docs/NATIVE-CPU.md): the game's
// functions as tools/rgcpu translated them, guest registers in host locals,
// stack frames as locals, direct calls with per-function signatures.
//
// With RG_NATIVE_CPU=1, install() puts each translated function's NAME_nc
// entry into the table slot that still holds ps2_recomp's own version; a
// slot a hook replaced keeps the hook (native code reaches it through its
// trampoline). Without the flag, or in a build without the generated
// native code, nothing changes.
namespace rg::nativecpu
{
void install(PS2Runtime &runtime);
}
