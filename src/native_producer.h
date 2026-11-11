#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct GSNativeMesh;
struct GSDrawState;
struct GSNativeEffect;

// The game side of the native pipeline (docs/NATIVE-PIPELINE.md): what turns
// the game's GS-shaped data into the renderer's records. The renderer sees
// ids only; the tables from the game's GS addresses to those ids are kept
// here, for the producers of draws (step 4). A producer may know the GS
// (this file does: the layouts the game uploads in); the renderer may not.
namespace rg::nativeproducer
{
// One mip level of an image node, as TexBuildUploadPkt sends it (game
// thread). `ram` is the level's bytes in EE memory, `ramBytes` how many
// are readable there.
struct TextureLevel
{
    const uint8_t *ram = nullptr;
    uint32_t ramBytes = 0;
    uint32_t tbp = 0;        // where the game puts it (a GS block), for the table
    uint32_t psm = 0;        // the texture's format (CT32, CT16, T8, T4)
    uint32_t width = 0;      // the texture's size in texels
    uint32_t height = 0;
    uint32_t bufferWidth = 0; // the texture's buffer width, in 64-pixel units
    // The transfer as the GS gets it: `transferWidth` by `transferHeight`
    // texels of `transferPsm` at `transferBufferWidth`. 8-bit art can go as
    // CT32 at half the width and height (`preSwizzled`), laid out so the
    // GS's memory reads it as T8.
    bool preSwizzled = false;
    uint32_t transferWidth = 0;
    uint32_t transferHeight = 0;
    uint32_t transferBufferWidth = 0;
    uint32_t transferPsm = 0;
    uint32_t cbp = 0; // the node's palette (TEX0's CBP), for an indexed texture
};
void texture(const TextureLevel &level);

// A palette as the node uploads it (game thread): 256 entries (a 16x16
// image, CSM1's order) or 16 (8x2), CT32 or CT16, to the GS block `cbp`.
void palette(const uint8_t *ram, uint32_t ramBytes, uint32_t cbp, uint32_t cpsm, uint32_t entries);

// Texture bindings (docs/NATIVE-BOUNDARY.md, issue 1): in place of a node's
// upload packets the texture hook writes a marker into the list, a GIF A+D
// packet of writes to a register the GS does not have. In guest memory each
// entry says where the level or palette is (BindEntry); the DMA send
// resolves it to the record's id (bindResolve, game thread, when the PS2's
// DMA would read the pixels), and the draw thread applies the resolved
// entries in the stream's order (bound). No upload is built, copied or
// withheld.
constexpr uint8_t kBindRegister = 0x7Eu;
// In the marker's DMA tag, bits 16-25 (unused by the DMA).
constexpr uint32_t kBindTagMagic = 0x2B5u;
constexpr uint8_t kBindPalette = 1u, kBindPreSwizzled = 2u;
struct BindEntry
{
    uint32_t source;  // the data, a physical address in guest RAM
    uint16_t width;   // the transfer's size in texels (TextureLevel's); a palette: its entries
    uint16_t height;
    uint8_t reg;      // kBindRegister (the A+D qword's register byte)
    uint8_t flags;    // kBindPalette, kBindPreSwizzled
    uint16_t at;      // the GS block: TBP, or the palette's CBP
    uint8_t psm;      // the texture's format, or the palette's CPSM
    uint8_t bufferWidth; // the texture's, in 64-pixel units
    uint16_t cbp;     // an indexed texture's palette block
};
static_assert(sizeof(BindEntry) == 16u, "an A+D qword");
// Whether a level is one a record can be made of (its format, its size, a
// pre-swizzled one's layout): the hook writes a marker for a node only when
// every level of it is, and leaves the others their uploads (game thread).
bool bindable(const TextureLevel &level);
// Once a list that holds markers, before its entries (game thread).
void bindBegin();
// The entry resolved: the A+D write's 64 bits (id, block, format, palette or
// not). An id of 0: no record could be made (counted), nothing is bound.
uint64_t bindResolve(const uint8_t *rdram, uint32_t ramSize, const BindEntry &entry);
// A marker's resolved writes, applied (the thread that follows the stream).
void bound(const uint64_t *values, size_t count);

// A render target the game names (gs_named_targets' kinds: 0 auto, 1 the
// frame, 2 the display buffer). Any thread.
void target(uint32_t base, uint32_t bufferWidth, uint32_t width, uint32_t height, uint32_t psm, int kind);

// The tables, for draw producers (any thread): the id of the latest texture
// at this block in this format, the palette at this block, the target at
// this base. 0 when none.
uint32_t textureAt(uint32_t tbp, uint32_t psm);
uint32_t paletteAt(uint32_t cbp);
uint32_t targetAt(uint32_t base);

// Step 4a: a VU1 program's mesh as the renderer's mesh draw (the VIF1
// thread, as the mesh goes to the GS). `vu1Data` is the VU1 data memory the
// mesh was built from: the camera (quads 4-11, 24-27) and the model (60-63)
// are read from it. `state` is the draw state the mesh is drawn under
// (src/draw_state_mirror.h); null when it has none (the mesh is counted, not
// made). `source` names what drew it, for profiles. True when the record
// was made: the mesh is then not sent to the GS (step 4c).
// `retained`: the mesh is one the renderer keeps under this id
// (docs/NATIVE-BOUNDARY.md, issue 2): with its vertices it is defined first,
// without them (a later draw) only the id goes.
bool mesh(const GSNativeMesh &mesh, const GSDrawState *state, const uint8_t *vu1Data, uint32_t source,
          uint32_t retained = 0u);
// A name for a `source`, for the renderer's draw labels (a frame capture's;
// debug/bug_mark.h). A VU1 mesh's source stands for its native program.
void nameSource(uint32_t source, const char *name);

// The renderer's epoch of retained meshes: when it changes, the ids handed
// out before are no longer drawn (the renderer let go of them all).
uint64_t retainedEpoch();

// Step 5a's survey (counts only): an effect on its way to be drawn (by
// command; a sprite effect's ops by what they draw into and from).
void noteEffect(const GSNativeEffect &effect);

// Step 5b: a 2D piece (src/native_2d.h: primitives the converter made a
// mesh in the GS's window coordinates) under its draw state, as the
// renderer's window-space mesh draw. `texels`: its UVs are texels (FST),
// not normalised. True when made; false leaves it to the GS (counted by
// why).
// `indices`: the mesh's triangles into mesh.vertices (empty: the vertices
// are a triangle list themselves).
bool piece(const GSNativeMesh &mesh, const GSDrawState *state, bool texels,
           const std::vector<uint32_t> &indices = {});

// Step 5b: an effect's sprites (src/native_effects.h) as the renderer's
// window-space mesh draws, and step 5c's object layer commands. True when
// made: the GS does not draw it then. False leaves it to the GS (counted by
// why): the effect's packets go on as they are.
bool effect(const GSNativeEffect &effect);

// Step 3b's counts (the VIF1 thread, which withholds named uploads the
// producer sent from the GS): one withheld (palette or texture), one sent on
// and why, an A+D block whose other registers went on without the
// transfer's, and image data that continued in a later packet.
// The VIF1 thread, for each upload on its way to the GS: whether this
// producer made a record of it (in the order the game built them; uploads
// not seen within a short lookahead were never sent and are dropped). Taken
// when it did: the upload is withheld, and `palette` says which it was.
// `hash` and `bytes`: the record's imageHash of the guest memory it was
// made from (what the upload sends, when the game has not changed it since).
bool takeProduced(uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t width, uint32_t height, bool &palette,
                  uint64_t &hash, uint32_t &bytes);

enum class Passed
{
    NotProduced, // no record of it here: not a named upload, or refused
};
// Step 6: a host upload nobody took (the absorber's, docs/NATIVE-PIPELINE.md
// "host uploads nobody took"), its pixels as sent (rows in order; 4-bit
// formats low nibble first). Kept as a linear image per base and format,
// which draws reading that format at that buffer width sample (sourceOf).
void hostUpload(uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                const uint8_t *data, uint32_t bytes);
// Step 6: a local copy (TRXDIR 2) the absorber took, logged with what the
// producer holds at its source and destination (the first few).
void noteLocalCopy(uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t sx, uint32_t sy, uint32_t dbp, uint32_t dbw,
                   uint32_t dpsm, uint32_t dx, uint32_t dy, uint32_t width, uint32_t height);
// A withheld upload whose data as sent differs from the guest memory its
// record was made from (the game wrote it after the texture hook): the
// record made again from `data` and resident in its place (VIF1 thread, in
// the stream's order). Palettes of 16 or 256 entries; textures uploaded as
// the format they are read in.
void replaceUpload(bool palette, uint32_t dbp, uint32_t dpsm, uint32_t width, uint32_t height, const uint8_t *data,
                   uint32_t bytes);
// The hash takeProduced's `hash` is of, for bytes the VIF1 thread saw sent.
uint64_t uploadHash(const uint8_t *data, size_t size);
void noteWithheld(bool palette, uint32_t imageBytes);
// Whether `size` bytes at `at` (guest RAM) are a texture level texture()
// hashed and no write has reached since (rg::writewatch); `hash` is then
// that hash, the one takeProduced gives for its record. On the game thread
// only. An upload sent from such memory carries nothing the record does not
// already hold: the DMA send leaves its bytes where they are (vif1_thread's
// elided spans).
bool sourceUnchanged(const uint8_t *at, size_t size, uint64_t &hash);
// An elided upload withheld without its bytes; `fetched`: one that needed
// them after all (read from guest memory, late).
void noteElided(uint32_t imageBytes, bool fetched);
void notePassed(Passed why);
void noteSplit();
void noteContinuedInPacket();

// Step 7b: a depth read the game makes (sceGsExecStoreImage of a Z buffer:
// the lens flare's 8x8 block at the sun), from the renderer's depth a frame
// or two late, in the read's format (Z32, Z24, Z16: little-endian, rows in
// order). `base` is the read's SBP, in blocks. Zeros (the clear's) where the
// renderer has no answer yet. False when the pipeline is off or the read is
// not of a depth format or too large: it then goes as before.
bool readDepth(uint32_t base, uint32_t psm, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
               std::vector<uint8_t> &out);

// Step 7c: the target a colour read of the GS buffer at `base` reads: the
// one drawn there, or the scene (for the display buffer, or a buffer never
// drawn).
uint32_t captureTarget(uint32_t base);

// The scene target's id (the frame's).
constexpr uint32_t kSceneTarget = 1u;

// M3 of docs/NATIVE-ONLY.md: a post effect as a Pass record (native_records.h),
// in the stream's order (the VIF1 thread, where the effect's packets were).
// Its frame and work buffers by the GS bases the game draws them at (frame
// 0: the scene); `kind` is native::PassKind's. False when a buffer is not a
// target the producer declared (counted).
struct PassRequest
{
    uint8_t kind = 0;
    uint32_t frameBase = 0;
    uint32_t workBases[4] = {};
    // A work buffer the game never declared (it set its frame directly):
    // declared at this size (CT32), when given.
    uint32_t workSizes[4][2] = {};
    float params[16] = {};
    // The params (a bit each) that are the GS's window Z, turned into the
    // renderer's depth through the frame's camera; and whether the pass
    // tests against the scene's depth.
    uint32_t depthParams = 0;
    bool sceneDepth = false;
};
bool pass(const PassRequest &request);

// After the flip's Present record: the next frame begins. Its Clear records
// (the scene's, and each depth target's at its first use) follow from here.
void frameBoundary();
} // namespace rg::nativeproducer
