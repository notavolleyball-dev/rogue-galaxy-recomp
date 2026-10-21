#pragma once

class PS2Runtime;

// The movies' frames sampled smooth.
//
// MovieDrawFrame (0x1D5870) draws each decoded frame (512x336 for 16:9,
// 512x448 for 4:3) as one sprite with the texture filter off, as the PS2
// did: a television blurred it, but at a raised render scale every texel
// is a hard block, and where Widescreen stretches 336 lines over 448 one
// line in three is doubled, so the edges come out in stairs. With the
// menu's game option `smooth_movies` (on unless RG_SMOOTH_MOVIES=0), the
// frame's GsPktSetTexFilter call asks for the bilinear filter instead; at 1x
// and 1:1 it samples the texel centres and draws the same picture.
namespace rg::movie
{

// After loadELF().
void installFilter(PS2Runtime &runtime);

void setSmooth(bool on);

} // namespace rg::movie
