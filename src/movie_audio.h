#pragma once

class PS2Runtime;

// Keeps every byte of movie audio.
//
// The movie player's PCM callback (0x001D4CA0) copies each audio packet into
// a 48 KB ring that the read thread drains to the IOP, and drops whatever
// does not fit. libmpeg on the console demuxes only as far ahead as the IPU
// decodes, so the ring never fills; the runtime's demux runs ahead by
// wall-clock time and fills it, and every dropped packet shifts the rest of
// the stream. Its samples are 16-bit, in 512-byte blocks per channel, so a
// shift that is odd turns the sound into full-scale noise and one that is
// not a whole frame swaps the channels block by block.
//
// The callback here takes every packet into a queue and moves what fits into
// the game's ring, and tops the ring up again each time the read thread has
// sent some of it to the IOP. RG_MOVIE_AUDIO=game leaves the game's callback.
namespace rg::movie
{

// After loadELF().
void install(PS2Runtime &runtime);

} // namespace rg::movie
