#pragma once

// The native pipeline (Rogue Galaxy: added; the game's docs/NATIVE-PIPELINE.md).
//
// Producers push records (native_records.h) from any thread, in the order
// the game built them; the renderer drains them on the thread that owns the
// GPU device. Step 2 of the plan: the renderer keeps the targets and cameras
// it is sent, counts how ready the draws are for the features (R1-R5), and
// presents its scene target cleared. Nothing of the GS here.

#include "gfx/native/native_records.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
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
    // What the presenter shows: the scene target, recorded into `commands`
    // (cleared, in step 2). False with `error` set when the GPU refused.
    struct Frame
    {
        SDL_GPUTexture *texture = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    bool render(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, uint32_t scale, Frame &out,
                std::string &error);

    // The window since the last call: records by kind, and R1-R5.
    std::string statsLine();

    void release(SDL_GPUDevice *device);

private:
    void apply(Record &record);
    void count(const MeshDraw &draw);

    std::vector<Record> m_drained;
    std::unordered_map<Id, Target> m_targets;
    std::unordered_map<Id, Camera> m_cameras;
    Id m_sceneTarget = 0;
    uint64_t m_records[std::variant_size_v<Record>] = {};
    uint64_t m_reported[std::variant_size_v<Record>] = {};
    Readiness m_readiness;
    Readiness m_readinessReported;
    SDL_GPUTexture *m_scene = nullptr;
    uint32_t m_sceneWidth = 0;
    uint32_t m_sceneHeight = 0;
};
} // namespace dq8::gfx::native
