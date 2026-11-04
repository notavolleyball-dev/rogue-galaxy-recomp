#include "gfx/native/native_pipeline.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <type_traits>

namespace dq8::gfx::native
{
namespace
{
std::atomic<bool (*)()> g_outer{nullptr};

struct Queue
{
    std::mutex mutex;
    std::vector<Record> records;
};
Queue &queue()
{
    static Queue instance;
    return instance;
}

// The scene target until a producer declares one (the game's frame size).
constexpr uint32_t kDefaultWidth = 640u;
constexpr uint32_t kDefaultHeight = 448u;

constexpr const char *kRecordNames[] = {"texture", "palette", "target",     "camera",   "mesh",      "sprite",
                                        "effect",  "frame",   "present",    "readback", "depth-query"};
static_assert(std::size(kRecordNames) == std::variant_size_v<Record>, "a name per record kind");
} // namespace

void setOuterSwitch(bool (*outer)())
{
    g_outer.store(outer, std::memory_order_release);
}

bool enabled()
{
    static const bool on = [] {
        bool (*outer)() = g_outer.load(std::memory_order_acquire);
        const char *v = std::getenv("RG_NATIVE_PIPELINE");
        const bool asked = v && *v && *v != '0';
        const bool result = asked && outer && outer();
        if (asked && !result)
            std::fprintf(stderr, "[native-pipeline] RG_NATIVE_PIPELINE ignored: the native renderer is off\n");
        else if (result)
            std::fprintf(stderr, "[native-pipeline] on\n");
        return result;
    }();
    return on;
}

void push(Record &&record)
{
    if (!enabled())
        return;
    Queue &q = queue();
    std::lock_guard lock(q.mutex);
    q.records.push_back(std::move(record));
}

void Renderer::count(const MeshDraw &draw)
{
    Readiness &r = m_readiness;
    ++r.meshDraws;
    if (draw.camera != 0u && draw.hasModel && m_cameras.count(draw.camera))
        ++r.cameraAndModel;
    if (draw.object != 0u)
        ++r.objectIdentity;
    if (const auto t = m_targets.find(draw.target);
        t != m_targets.end() && t->second.role == TargetRole::SceneColor && draw.floatDepth)
        ++r.sceneFloatDepth;
    if (draw.material.known)
        ++r.materialKnown;
    if (draw.hasNormals)
        ++r.normals;
}

void Renderer::apply(Record &record)
{
    ++m_records[record.index()];
    std::visit(
        [this](auto &r) {
            using T = std::decay_t<decltype(r)>;
            if constexpr (std::is_same_v<T, Target>)
            {
                m_targets[r.id] = r;
                if (r.role == TargetRole::SceneColor)
                    m_sceneTarget = r.id;
            }
            else if constexpr (std::is_same_v<T, Camera>)
                m_cameras[r.id] = r;
            else if constexpr (std::is_same_v<T, MeshDraw>)
                count(r);
            // The rest is drawn from step 3 on; counted above.
        },
        record);
}

bool Renderer::render(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, uint32_t scale, Frame &out,
                      std::string &error)
{
    {
        Queue &q = queue();
        std::lock_guard lock(q.mutex);
        m_drained.swap(q.records);
    }
    for (Record &record : m_drained)
        apply(record);
    m_drained.clear();

    uint32_t width = kDefaultWidth, height = kDefaultHeight;
    if (const auto t = m_targets.find(m_sceneTarget); t != m_targets.end() && t->second.width && t->second.height)
    {
        width = t->second.width;
        height = t->second.height;
    }
    scale = scale ? scale : 1u;
    width *= scale;
    height *= scale;
    if (!m_scene || m_sceneWidth != width || m_sceneHeight != height)
    {
        release(device);
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        m_scene = SDL_CreateGPUTexture(device, &info);
        if (!m_scene)
        {
            error = std::string("SDL_CreateGPUTexture(native scene): ") + SDL_GetError();
            return false;
        }
        m_sceneWidth = width;
        m_sceneHeight = height;
    }

    // Step 2: the scene cleared, a dark blue that no game frame is, so the
    // switch is visible on screen.
    SDL_GPUColorTargetInfo target{};
    target.texture = m_scene;
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.clear_color = SDL_FColor{0.05f, 0.08f, 0.20f, 1.0f};
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr);
    if (!pass)
    {
        error = std::string("SDL_BeginGPURenderPass(native scene): ") + SDL_GetError();
        return false;
    }
    SDL_EndGPURenderPass(pass);

    out.texture = m_scene;
    out.width = m_sceneWidth;
    out.height = m_sceneHeight;
    return true;
}

std::string Renderer::statsLine()
{
    std::string line = "[native-pipeline] records:";
    for (size_t i = 0; i < std::size(kRecordNames); ++i)
    {
        line += std::string(" ") + kRecordNames[i] + " " + std::to_string(m_records[i] - m_reported[i]);
        m_reported[i] = m_records[i];
    }
    const Readiness &now = m_readiness, &was = m_readinessReported;
    const auto d = [](uint64_t a, uint64_t b) { return std::to_string(a - b); };
    line += " | readiness of " + d(now.meshDraws, was.meshDraws) + " mesh draws: R1 camera+model " +
            d(now.cameraAndModel, was.cameraAndModel) + ", R2 object " + d(now.objectIdentity, was.objectIdentity) +
            ", R3 scene float depth " + d(now.sceneFloatDepth, was.sceneFloatDepth) + ", R4 material " +
            d(now.materialKnown, was.materialKnown) + ", R5 normals " + d(now.normals, was.normals) + "\n";
    m_readinessReported = m_readiness;
    return line;
}

void Renderer::release(SDL_GPUDevice *device)
{
    if (m_scene && device)
        SDL_ReleaseGPUTexture(device, m_scene);
    m_scene = nullptr;
    m_sceneWidth = m_sceneHeight = 0;
}
} // namespace dq8::gfx::native
