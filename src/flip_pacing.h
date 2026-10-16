#pragma once

#include <memory>

class GSRasterBackend;
class PS2Runtime;

// Presents the frames the game flips, one each, in order.
//
// The runtime's presenter samples the display at host vblank ticks and waits
// for the GS worker to reach each sample. With the worker busy, those samples
// land at uneven times against the game's 30 Hz flips, so some frames show
// twice and others never, and characters visibly skip frames. Here the
// presentation is instead prepared on the game's thread right after each
// GfxFlipBuffers (0x00143650), at that point in the GS command stream, and
// the presenter shows those. Without flips for a while (movies, loading
// screens) it presents as before.
//
// RG_PRESENT=vblank turns it off.
namespace rg::graphics
{

// Wraps the backend the GS frontend talks to (the worker queue, when there
// is one). Returns it unchanged when disabled.
std::unique_ptr<GSRasterBackend> paceOnFlips(std::unique_ptr<GSRasterBackend> backend);

// After loadELF(): hooks the game's flip function.
void installFlipHook(PS2Runtime &runtime);

} // namespace rg::graphics
