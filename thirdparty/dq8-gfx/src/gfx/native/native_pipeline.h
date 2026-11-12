#pragma once

// The native pipeline (Rogue Galaxy: added; the game's docs/NATIVE-PIPELINE.md).
//
// Producers push records (native_records.h) from any thread, in the order
// the game built them; the renderer drains them on the thread that owns the
// GPU device. So far (step 3a of the plan) the renderer keeps the targets,
// cameras, textures and palettes it is sent, counts how ready the draws are
// for the features (R1-R5), and presents its scene target with the mesh
// draws of the last frame a Present record closed (step 4b). Nothing of the
// GS here.

#include "gfx/native/native_records.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dq8::gfx::native
{
// Any thread.
void push(Record &&record);

// What every color target is on the GPU: float, so the draw's units past
// 1.0 survive (records.h).
inline constexpr SDL_GPUTextureFormat kColorFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

// The block-compressed formats the renderer's device samples, or'd: 1 BC1,
// 2 BC2, 4 BC3, 8 BC7. 0 until the renderer has first run. Any thread.
int blockFormats();

// Debug: with draw labels on, the renderer puts a label before each mesh
// call in the command buffer, for a frame capture (RenderDoc) to show:
//   rg f=FRAME n=FIRST-LAST tgt=T dt=D tex=X stex=S obj=O cam=C model=0|1 src=NAME
// FRAME and n are the depth log's (its F and D lines), the ids the records',
// NAME what nameSource() gave MeshDraw::source (else its hex). Any thread.
void setDrawLabels(bool on);
void nameSource(uint32_t source, const char *name);

// A mesh draw's vertex and index buffers, empty but with the capacity of one
// the renderer has drawn and let go of, so a producer makes its draws
// without the allocator. The renderer frees nothing it was handed: freeing
// on its thread what the producer's allocated took the producer's malloc
// arena lock, and the two threads spent most of their time waiting on it
// (2026-11-08: the presenter blocked ~4,400 times a second in delete).
// Any thread; the producer's side takes a batch at a time.
void reuseMeshBuffers(MeshDraw &draw);
void reuseMeshBuffers(Draw2D &draw);

// A retained mesh's vertex (native_mesh_static.vert): its triangle's three
// corners, its own UV and colour, which corner it is and the triangle's
// place in the strip (the sign its winding has there).

// A model chunk's triangle vertex (Renderer::retain): its triangle's three
// corners' positions, bones and weights (the program's backface rule needs
// all three, through the same skinning), its own normal, UV and color.
struct ModelVertex
{
    float position[3][3];
    float bones[3][4];
    float weights[3][4];
    float normal[3];
    float uv[2];
    float color[4];
    float corner, parity;
};
static_assert(sizeof(ModelVertex) == 176u, "tightly packed");
// A side of the model chunks' buffer (two: Renderer::retain). 88 MiB.
constexpr uint32_t kModelVertices = 1u << 19;
uint32_t modelTriangleCount(size_t count, bool list);
void modelTriangles(const std::vector<ModelInput> &in, bool list, ModelVertex *out);
// An outline draw's (MeshDraw::modelEdge): 24 vertices an edge, its four
// lines two triangles each. Each vertex: A, B, C the far corner of the
// first triangle (B again for an edge alone), the edge's two ends; the
// second triangle's far corner in normal (its position), uv (its bones, two
// a value: b0 + 256 b1, b2 + 256 b3) and color (its weights); corner.x the
// vertex (line x 6 + which of the six), corner.y the points an edge.
constexpr uint32_t kOutlineEdgeVertices = 24u;
void outlineVertices(const std::vector<ModelInput> &in, uint32_t edge, ModelVertex *out);
// A shadow volume draw's (MeshDraw::modelVolume): 30 vertices a triangle of
// its list, its volume's five fans of two triangles, each with the
// triangle's three corners; corner.x the vertex (fan x 6 + which of six).
constexpr uint32_t kVolumeTriangleVertices = 30u;
void volumeVertices(const std::vector<ModelInput> &in, ModelVertex *out);

// Bumped when the renderer lets go of every retained mesh (its buffer
// filled): the ids the producer handed out before are no longer drawn, and
// it defines its meshes again under new ones. Any thread.
uint64_t retainedEpoch();
// RG_FLASH_WATCH=1 (Renderer's flash watch).
bool watchOn();
uint64_t watchHash(const void *data, size_t bytes, uint64_t h = 1469598103934665603ull);
void retainedReset(); // the renderer's own: a new epoch

// Textures the renderer let go of (its memory budget): a producer that sends
// a texture once and names it by id afterwards drains these and sends them
// again when next used. Any thread.
std::vector<Id> takeEvicted();

// Step 7b: depth reads (the game's lens flare tests the sun against the
// scene's depth), answered a frame or two late, without waiting for the GPU.
// The caller asks and takes the last answer for a read in its place: reads
// are told apart by their order since the renderer last took them, and an
// answer is used only for a block of the same target and size. The
// renderer, at each frame it draws, publishes the blocks it copied out of
// its depth for the reads before, and copies out what was asked since.
struct DepthBlock
{
    Id target = 0; // a depth target
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0; // in the game's pixels
};
constexpr uint32_t kMaxDepthReads = 4u; // a frame's
constexpr uint32_t kMaxDepthSide = 64u;
// Any thread: the block's depth, reversed (1 nearest, 0 the clear), one
// value a game pixel (the nearest of the pixels it covers), rows in order.
// False when there is no answer for it yet.
bool readDepth(const DepthBlock &block, std::vector<float> &out);

// Step 7c: the game's colour reads (the fade's and the menu's frame
// captures), from what the renderer drew last, each game pixel the mean of
// the pixels it covers. The caller opens a read and waits; the renderer's
// owner's presenting thread, which runs beside the game's (step 7d), sees
// colorReadsWaiting and calls Renderer::serveColorReads.
struct ColorRead
{
    Id target = 0;
    Rect rect; // in the target's game pixels
    // When set: the same pixels also kept on the GPU in this target, at the
    // renderer's resolution, for a texture made of the game's copy later
    // (Texture::fromTarget), so the frame a fade lays over stays as sharp
    // as it was drawn.
    Id keep = 0;
};
uint32_t beginColorRead(const ColorRead &read);
// Any thread: a read was opened and not answered yet.
bool colorReadsWaiting();
// Four floats a pixel in the target's units, rows in order. False when the
// renderer could not serve it, or did not within `timeoutMs`.
bool waitColorRead(uint32_t id, std::vector<float> &rgba, uint32_t timeoutMs);

// A line of the producers' own counts, printed with the renderer's.
void setProducerStats(std::function<std::string()> line);

// The readiness counters, docs/NATIVE-PIPELINE.md's R1-R5, over the mesh
// draws drained.
struct Readiness
{
    uint64_t meshDraws = 0;
    uint64_t cameraAndModel = 0; // R1
    uint64_t objectIdentity = 0; // R2
    uint64_t sceneFloatDepth = 0; // R3
    uint64_t materialKnown = 0;  // R4
    uint64_t normals = 0;        // R5
};

class Renderer
{
public:
    // Step 7c: answers every colour read waiting, from the targets as the
    // last frame drawn left them; waits for the GPU. Anything the owner has
    // pending must be submitted first.
    void serveColorReads(SDL_GPUDevice *device);
    // The triggered dump's colour targets, as PPM in build/run/targets
    // (colour, and alpha beside it), the frame after drawFrame asked.
    void dumpTargets(SDL_GPUDevice *device);
    bool dumpPending() const { return m_dumpTargets; }

    // What the presenter shows: the scene target, recorded into `commands`.
    // False with `error` set when the GPU refused.
    struct Frame
    {
        SDL_GPUTexture *texture = nullptr;
        uint32_t x = 0, y = 0; // what of the texture is shown, from here
        uint32_t width = 0;
        uint32_t height = 0;
    };
    bool render(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, uint32_t scale, Frame &out,
                std::string &error);

    // The window since the last call: records by kind, textures held, and R1-R5.
    std::string statsLine();
    // Totals since the start (and what is held now), for the menu's page.
    struct Totals
    {
        uint64_t frames = 0, meshesDrawn = 0, gpuCalls = 0, passes = 0, copies = 0;
        uint64_t texturesHeld = 0, textureBytes = 0, retainedMeshes = 0;
    };
    Totals totals() const;

    void release(SDL_GPUDevice *device);

private:
    struct Image
    {
        SDL_GPUTexture *texture = nullptr;
        PixelFormat format = PixelFormat::Rgba8;
        uint32_t width = 0;
        uint32_t height = 0;
        Id palette = 0;
        uint64_t bytes = 0;
        uint32_t logicalWidth = 0, logicalHeight = 0; // the game's size, when it differs
        float scale = 1.0f; // Texture::scale, Palette::scale
    };
    struct PendingUpload
    {
        SDL_GPUTexture *texture = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        Bytes bytes;
    };

    void apply(SDL_GPUDevice *device, Record &record, std::string &error);
    void count(const MeshDraw &draw);
    // A Pass as the renderer's own draws and clears, in order into the frame
    // being gathered (native_passes.cpp). False: a kind not drawn yet, or
    // targets not declared (counted).
    bool expandPass(const Pass &pass);
    // One rectangle of a pass: `source` (0: none, untextured) from its texels
    // u0..u1, v0..v1 into `target`'s pixels x0..x1, y0..y1, times `color`
    // (each channel), with `material`'s blend, tests and filter.
    void passRect(Id target, Id source, const float rect[4], const float uv[4], float color,
                  const Material &material);
    void passRect(Id target, Id source, const float rect[4], const float uv[4], const float rgba[4],
                  const Material &material);
    // A target of the renderer's own for a pass (a ping-pong partner),
    // declared like `like` the first time.
    Id passScratch(uint32_t slot, Id like);
    // CT16 texels (PixelFormat::Ct16) as the device samples them, and their
    // bytes for it (native_pipeline.cpp).
    SDL_GPUTextureFormat ct16Format(SDL_GPUDevice *device);
    Bytes ct16Texels(SDL_GPUDevice *device, const Bytes &raw, size_t count);
    int m_ct16Native = -1;
    uint64_t m_passesDrawn = 0, m_passesRefused = 0;
    SDL_GPUTexture *createTexture(SDL_GPUDevice *device, SDL_GPUTextureFormat format, uint32_t width,
                                  uint32_t height, std::string &error);
    bool upload(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, std::string &error);
    bool ensureView(SDL_GPUDevice *device, std::string &error);
    void drawView(SDL_GPUCommandBuffer *commands, SDL_GPURenderPass *pass);
    void evict(SDL_GPUDevice *device);

    // Step 4b's mesh draws (native_meshes.cpp): the frame's draws into the
    // scene target, with a depth target of its own.
    bool ensureMeshes(SDL_GPUDevice *device, std::string &error);
    SDL_GPUGraphicsPipeline *meshPipeline(SDL_GPUDevice *device, uint32_t key);
    SDL_GPUSampler *meshSampler(SDL_GPUDevice *device, uint32_t key);
    bool uploadMeshes(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, std::string &error);
    // The frame's draws in order, a render pass per run of draws into one
    // target (step 5a). Planned first (every draw's state resolved, its
    // uniforms written to the frame's draw table, every texture made), then
    // uploaded with the vertices, then recorded: a draw reads its uniforms
    // from the table by the draw id its vertices carry, so neighbours that
    // bind the same state go to the GPU as one call.
    bool drawFrame(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, uint32_t scale, std::string &error);
    struct PassPlan
    {
        Id target = 0, depthId = 0;
        SDL_GPUTexture *color = nullptr, *depth = nullptr;
        SDL_GPUTexture *copy = nullptr; // the color copied here first (a draw reads its own target)
        // Instead of draws: a Clear record, its texture (color, or depth).
        bool clear = false;
        Clear clearRecord;
        uint32_t colorW = 0, colorH = 0, gameW = 0, gameH = 0;
        SDL_GPULoadOp colorLoad = SDL_GPU_LOADOP_LOAD, depthLoad = SDL_GPU_LOADOP_LOAD;
        size_t first = 0, last = 0;
    };
    void planDraws(SDL_GPUDevice *device, const PassPlan &pass, uint32_t scale, uint32_t copyW, uint32_t copyH);
    void recordPass(SDL_GPUCommandBuffer *commands, SDL_GPURenderPass *pass, const PassPlan &plan);
    // m_gpuTargets' key for the copy a draw reading its own target reads.
    static constexpr Id kFeedbackCopy = 0xFFFFFFFFu;
    // A target's GPU texture (color, or depth), at least `minW` by `minH`
    // pixels: kept across frames, grown (and so cleared) when too small.
    SDL_GPUTexture *targetTexture(SDL_GPUDevice *device, Id id, bool depth, uint32_t minW, uint32_t minH,
                                  bool &fresh, std::string &error);
    void releaseMeshes(SDL_GPUDevice *device);
    // Step 7b: publishes the depth blocks the GPU copied out, and copies out
    // the ones asked since (from the last frame drawn, before this one's
    // draws), in a command buffer of its own.
    void serveDepth(SDL_GPUDevice *device);
    void releaseDepth(SDL_GPUDevice *device);
    struct MeshCounts
    {
        uint64_t drawn = 0, offscreen = 0, noTarget = 0, depthAsColor = 0, depthMismatch = 0, passes = 0;
        uint64_t noCamera = 0, noTexture = 0, noPipeline = 0, frames = 0;
        uint64_t window = 0, fromTarget = 0, sourceNotDrawn = 0, feedback = 0, copies = 0, destinationAlpha = 0;
        uint64_t calls = 0; // GPU draw calls the drawn meshes took
    };
    // A draw's uniforms as the mesh shaders read them from the frame's draw
    // table (std430: every member 16-byte aligned).
    struct GpuDraw
    {
        // The vertex stage's.
        float clip[16];
        float viewport[16];
        float target[4];
        float fog[4];
        float depthRow[4];   // the depth (times w) straight from the position, scaled and biased
        float depthLimit[4]; // x: MeshDraw::depthCap (times w); y: 1 when it applies
        // The fragment stage's.
        uint32_t mode[4];
        uint32_t test[4];
        float size[4];
        float blend[4];
        float fogColor[4];
        float texa[4];
        float offset[4]; // xy: Material::texelOffset; zw: texaLow, texaHigh
        // A retained mesh's cull and colour cap (native_mesh_static.vert).
        float cullMatrix[16];
        float bandScale[4];
        float bandOffset[4];
        float cull[4]; // the winding sign; the guard band test, the backface test, a clipped chunk (0 or 1)
        float colorMax[4];
        float units[4];       // texel scale, color clamp, alpha reference, shuffle scale
        float vertexUnits[4]; // raw color scale
    };
    // A draw's binding state, resolved before recording.
    struct DrawPlan
    {
        SDL_GPUGraphicsPipeline *pipeline = nullptr; // null: not drawn
        SDL_GPUTexture *image = nullptr, *palette = nullptr;
        SDL_GPUSampler *sampler = nullptr;
        SDL_Rect scissor{};
        float blendConstant = -1.0f; // set when not negative
    };
    struct GpuTarget
    {
        SDL_GPUTexture *texture = nullptr;
        uint32_t width = 0, height = 0; // allocated, in host pixels
        uint32_t scale = 0;
    };

    std::vector<Record> m_drained;
    std::unordered_map<Id, Target> m_targets;
    std::unordered_map<Id, Camera> m_cameras; // the newest 2 * kCameraWindow ids at most
    Id m_newestCamera = 0;
    std::unordered_map<Id, Image> m_images;   // textures
    std::unordered_map<Id, Image> m_palettes; // as 16x1 or 256x1 RGBA
    std::deque<Id> m_imageOrder;              // oldest first, for the budget
    uint64_t m_imageBytes = 0;
    std::vector<PendingUpload> m_uploads;
    // Textures made from a kept target (Texture::fromTarget): copied at the
    // next upload.
    struct PendingCopy
    {
        SDL_GPUTexture *from = nullptr, *to = nullptr;
        uint32_t width = 0, height = 0;
    };
    std::vector<PendingCopy> m_targetCopies;
    std::deque<Id> m_recent; // the newest textures, for the view
    Id m_sceneTarget = 0;
    uint64_t m_records[std::variant_size_v<Record>] = {};
    uint64_t m_reported[std::variant_size_v<Record>] = {};
    uint64_t m_evicted = 0, m_evictedReported = 0;
    uint64_t m_refused = 0, m_refusedReported = 0;
    Readiness m_readiness;
    Readiness m_readinessReported;
    SDL_GPUTexture *m_scene = nullptr;
    uint32_t m_sceneWidth = 0;
    uint32_t m_sceneHeight = 0;
    SDL_GPUGraphicsPipeline *m_viewPipeline = nullptr;
    SDL_GPUSampler *m_viewSampler = nullptr;
    bool m_viewFailed = false;
    bool m_textureGrid = false; // RG_NATIVE_TEXTURE_GRID=1: step 3a's view over the scene

    std::vector<MeshDraw> m_meshes;      // drawn since the last Present record, any target
    std::vector<MeshDraw> m_frameMeshes; // the frame a Present record closed
    // The Clear records among them, each before the draw of its index.
    std::vector<std::pair<size_t, Clear>> m_clears, m_frameClears;
    // What the last Present record shows.
    Id m_presentTarget = 0;
    Rect m_presentRect;
    float m_presentScale = 1.0f;
    // The picture shown: the Present record's rectangle times its scale, in
    // 8 bits (resolve), what render() hands the presenter.
    bool resolve(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, SDL_GPUTexture *source, uint32_t x,
                 uint32_t y, uint32_t width, uint32_t height, std::string &error);
    SDL_GPUTexture *m_output = nullptr;
    uint32_t m_outputWidth = 0, m_outputHeight = 0;
    SDL_GPUGraphicsPipeline *m_resolvePipeline = nullptr;
    SDL_GPUSampler *m_resolveSampler = nullptr;
    bool m_resolveFailed = false;
    uint64_t m_clearsRefused = 0;
    bool m_frameReady = false;
    bool m_dumpTargets = false; // the triggered dump: write the targets out
    SDL_GPUShader *m_meshVertex = nullptr;
    SDL_GPUShader *m_meshFragment = nullptr;
    bool m_meshFailed = false;
    SDL_GPUTextureFormat m_depthFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
    std::unordered_map<Id, GpuTarget> m_gpuTargets; // offscreen colors, and every depth (by its id)
    SDL_GPUTexture *m_white = nullptr;
    std::unordered_map<uint32_t, SDL_GPUGraphicsPipeline *> m_meshPipelines;
    SDL_GPUSampler *m_meshSamplers[8] = {};
    SDL_GPUBuffer *m_vertexBuffer = nullptr;
    SDL_GPUBuffer *m_indexBuffer = nullptr;
    uint32_t m_vertexCapacity = 0, m_indexCapacity = 0;
    SDL_GPUTransferBuffer *m_meshTransfer = nullptr;
    uint32_t m_transferCapacity = 0;
    SDL_GPUBuffer *m_drawBuffer = nullptr; // the frame's draw table
    uint32_t m_drawCapacity = 0;
    std::vector<uint32_t> m_firstIndex; // per frame mesh, into the index buffer
    // Kept model chunks (their triangles in m_modelBuffer), by id.
    struct Retained
    {
        uint32_t first = 0, count = 0; // vertices
        bool model = false;            // a model chunk's, in m_modelBuffer (ModelVertex)
        bool transient = false;        // this frame's alone, in m_transientBuffer
        uint8_t side = 0;              // which of m_modelBuffers
        uint64_t hash = 0;             // of its inputs (the flash watch's)
    };
    // The frame mesh `n`'s: a retained mesh's or model chunk's, or a model
    // chunk drawn this once (m_transient).
    const Retained *retainedAt(size_t n) const
    {
        if (n < m_transient.size() && m_transient[n].count != 0u)
            return &m_transient[n];
        return n < m_frameMeshes.size() ? retainedOf(m_frameMeshes[n]) : nullptr;
    }
    void retain(const RetainedMesh &mesh);
    const Retained *retainedOf(const MeshDraw &draw) const
    {
        if (draw.retained == 0u)
            return nullptr;
        const auto it = m_retained.find(draw.retained);
        return it != m_retained.end() ? &it->second : nullptr;
    }
    std::unordered_map<Id, Retained> m_retained;
    // RG_FLASH_WATCH=1 (a diagnostic): each drawn frame's picture, small,
    // and what its draws were; a frame whose picture differs from both its
    // neighbours where they agree is reported with the draws only it has
    // or lacks (native_pipeline.cpp, "the flash watch").
    struct WatchSig
    {
        uint64_t geometry = 0, pose = 0;
        uint32_t target = 0, state = 0, source = 0;
        Id texture = 0, palette = 0, sourceTarget = 0;
    };
    struct WatchFrame
    {
        uint64_t frame = 0;
        std::vector<WatchSig> sigs;
        std::vector<uint8_t> thumb; // luminance, kWatchW x kWatchH
        float camera[16] = {};
        uint32_t transient = 0, clears = 0;
        SDL_GPUTransferBuffer *download = nullptr;
        bool judged = false;
    };
    static constexpr uint32_t kWatchW = 64u, kWatchH = 56u;
    std::vector<WatchFrame> m_watch;
    WatchFrame m_watchPlanned;
    std::vector<SDL_GPUTexture *> m_watchMips;
    uint32_t m_watchMipW = 0, m_watchMipH = 0, m_watchReports = 0;
    void watchPlan();
    void watchShot(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands);
    void watchJudge(SDL_GPUDevice *device);
    uint64_t m_retainedDefined = 0, m_retainedDrawn = 0, m_retainedMissing = 0, m_retainedResets = 0;
    // Model chunks (M5): their triangles as ModelVertex in their own buffer,
    // drawn by native_model.vert; each draw's constants (ModelRow) and its
    // bones' matrices in two storage buffers made each frame.
    // Two sides: a full one is left as it is (the draws already made
    // against its ids still find them) and the other, emptied, takes the
    // new epoch's.
    SDL_GPUBuffer *m_modelBuffers[2] = {nullptr, nullptr};
    uint32_t m_modelSide = 0;
    SDL_GPUShader *m_modelVertex = nullptr;
    uint32_t m_modelUsed[2] = {0, 0}, m_modelUploaded[2] = {0, 0};
    std::vector<uint8_t> m_modelPending[2];
    std::vector<ModelRow> m_modelRows;  // the frame's
    std::vector<float> m_paletteData;   // the frame's, 16 floats a bone
    SDL_GPUBuffer *m_modelRowBuffer = nullptr, *m_paletteBuffer = nullptr;
    uint32_t m_modelRowCapacity = 0, m_paletteCapacity = 0;
    uint64_t m_modelDefined = 0, m_modelDrawn = 0, m_modelDrawnReported = 0;
    // Model chunks drawn this once: their triangles, made per frame.
    std::vector<Retained> m_transient;        // per frame mesh
    std::vector<ModelVertex> m_transientVertices;
    SDL_GPUBuffer *m_transientBuffer = nullptr;
    uint32_t m_transientCapacity = 0;
    uint64_t m_modelTransient = 0, m_modelTransientReported = 0;
    std::vector<GpuDraw> m_drawTable;   // per frame mesh
    std::vector<DrawPlan> m_drawPlans;  // per frame mesh
    std::vector<PassPlan> m_passPlans;
    // Textures replaced while a frame is planned, released once it is
    // recorded (a draw planned earlier may still bind one).
    std::vector<SDL_GPUTexture *> m_retired;
    MeshCounts m_meshCounts, m_meshReported;
    // Step 7b's depth reads in flight (one copy at a time).
    SDL_GPUTransferBuffer *m_depthDownload = nullptr;
    uint32_t m_depthDownloadBytes = 0;
    SDL_GPUFence *m_depthFence = nullptr;
    std::vector<std::pair<uint32_t, DepthBlock>> m_depthCopied; // the read's index, its block
    uint32_t m_depthCopyScale = 1;
    uint32_t m_frameScale = 0; // the scale the last frame was drawn at; 0 none yet
    bool m_depthFailed = false;
    struct DepthCounts
    {
        uint64_t asked = 0, copied = 0, published = 0, noTarget = 0, waiting = 0, unserved = 0;
    };
    DepthCounts m_depthCounts, m_depthReported;
    uint64_t m_colorReads = 0, m_colorReadsFailed = 0, m_colorReadsReported = 0, m_colorFailedReported = 0;
    uint64_t m_keptTextures = 0, m_keptTexturesReported = 0; // textures made from a kept colour read
};
} // namespace dq8::gfx::native
