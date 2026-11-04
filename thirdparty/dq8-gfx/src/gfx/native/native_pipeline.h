#pragma once

// The native pipeline (Rogue Galaxy: added; the game's docs/NATIVE-PIPELINE.md).
//
// Producers push records (native_records.h) from any thread, in the order
// the game built them; the renderer drains them on the thread that owns the
// GPU device. So far (step 3a of the plan) the renderer keeps the targets,
// cameras, textures and palettes it is sent, counts how ready the draws are
// for the features (R1-R5), and presents its scene target: cleared, with
// the textures it holds drawn over it as a grid (the debug view, until it
// draws the game). Nothing of the GS here.

#include "gfx/native/native_records.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dq8::gfx::native
{
// The switch: RG_NATIVE_PIPELINE=1, and only under the game's native
// renderer flag, which the game provides as `outer` (it is decided once,
// after the game's settings are read). Decided at the first enabled() call
// after setOuterSwitch; false until a query is set.
void setOuterSwitch(bool (*outer)());
bool enabled();

// Any thread. Dropped when the pipeline is off.
void push(Record &&record);

// Textures the renderer let go of (its memory budget): a producer that sends
// a texture once and names it by id afterwards drains these and sends them
// again when next used. Any thread.
std::vector<Id> takeEvicted();

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
    // What the presenter shows: the scene target, recorded into `commands`.
    // False with `error` set when the GPU refused.
    struct Frame
    {
        SDL_GPUTexture *texture = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    bool render(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, uint32_t scale, Frame &out,
                std::string &error);

    // The window since the last call: records by kind, textures held, and R1-R5.
    std::string statsLine();

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
    SDL_GPUTexture *createTexture(SDL_GPUDevice *device, SDL_GPUTextureFormat format, uint32_t width,
                                  uint32_t height, std::string &error);
    bool upload(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, std::string &error);
    bool ensureView(SDL_GPUDevice *device, std::string &error);
    void drawView(SDL_GPUCommandBuffer *commands, SDL_GPURenderPass *pass);
    void evict(SDL_GPUDevice *device);

    std::vector<Record> m_drained;
    std::unordered_map<Id, Target> m_targets;
    std::unordered_map<Id, Camera> m_cameras;
    std::unordered_map<Id, Image> m_images;   // textures
    std::unordered_map<Id, Image> m_palettes; // as 16x1 or 256x1 RGBA
    std::deque<Id> m_imageOrder;              // oldest first, for the budget
    uint64_t m_imageBytes = 0;
    std::vector<PendingUpload> m_uploads;
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
};
} // namespace dq8::gfx::native
