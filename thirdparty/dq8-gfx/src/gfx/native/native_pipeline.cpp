#include "gfx/native/native_pipeline.h"

#include "native_resolve.frag.spv.h"
#include "native_resolve.vert.spv.h"
#include "native_view.frag.spv.h"
#include "native_view.vert.spv.h"

#include <algorithm>
#include <filesystem>
#include <tuple>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <type_traits>

namespace dq8::gfx::native
{
namespace
{
struct Queue
{
    std::mutex mutex;
    std::vector<Record> records;
    std::vector<Id> evicted;
    std::function<std::string()> producerStats;
};
Queue &queue()
{
    static Queue instance;
    return instance;
}

// The scene target until a producer declares one (the game's frame size).
constexpr uint32_t kDefaultWidth = 640u;
constexpr uint32_t kDefaultHeight = 448u;
// The textures held, past which the oldest are let go (and their producers
// told, takeEvicted).
constexpr uint64_t kImageBudget = 256ull << 20;
// The debug view's grid.
constexpr uint32_t kViewColumns = 8u;
constexpr uint32_t kViewRows = 6u;

constexpr const char *kRecordNames[] = {"texture", "palette", "target", "camera",  "mesh",          "2d",
                                        "pass",    "clear",   "frame",  "present", "retained-mesh"};
static_assert(std::size(kRecordNames) == std::variant_size_v<Record>, "a name per record kind");

// Step 7b's depth reads: what was asked since the renderer last took them,
// and the last answer for each place in that order.
struct DepthMailbox
{
    std::mutex mutex;
    std::vector<DepthBlock> wanted;
    uint32_t asked = 0; // reads since the renderer last took them
    struct Answer
    {
        DepthBlock block;
        std::vector<float> values;
        bool valid = false;
    };
    Answer answers[kMaxDepthReads];
};
DepthMailbox &depthMailbox()
{
    static DepthMailbox instance;
    return instance;
}

std::vector<DepthBlock> takeDepthReads()
{
    DepthMailbox &m = depthMailbox();
    std::lock_guard lock(m.mutex);
    m.asked = 0u;
    std::vector<DepthBlock> out;
    out.swap(m.wanted);
    return out;
}

void publishDepth(uint32_t index, const DepthBlock &block, std::vector<float> &&values)
{
    if (index >= kMaxDepthReads)
        return;
    DepthMailbox &m = depthMailbox();
    std::lock_guard lock(m.mutex);
    m.answers[index].block = block;
    m.answers[index].values = std::move(values);
    m.answers[index].valid = true;
}
// Step 7c's colour reads, by id.
struct ColorMailbox
{
    std::mutex mutex;
    std::condition_variable done;
    uint32_t next = 1;
    struct Entry
    {
        ColorRead read;
        bool finished = false, ok = false;
        std::vector<float> rgba;
    };
    std::unordered_map<uint32_t, Entry> entries;
};
ColorMailbox &colorMailbox()
{
    static ColorMailbox instance;
    return instance;
}

// An IEEE half (a color target's channel, kColorFormat) as a float.
float halfToFloat(uint16_t h)
{
    const uint32_t sign = uint32_t(h >> 15) << 31, exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
    uint32_t bits;
    if (exponent == 0u)
    {
        if (mantissa == 0u)
            bits = sign;
        else
        {
            // Subnormal: normalize.
            uint32_t m = mantissa, e = 113u;
            while ((m & 0x400u) == 0u)
            {
                m <<= 1u;
                --e;
            }
            bits = sign | (e << 23) | ((m & 0x3FFu) << 13);
        }
    }
    else if (exponent == 31u)
        bits = sign | 0x7F800000u | (mantissa << 13);
    else
        bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}
} // namespace

uint32_t beginColorRead(const ColorRead &read)
{
    ColorMailbox &m = colorMailbox();
    std::lock_guard lock(m.mutex);
    uint32_t id = m.next++;
    if (id == 0u) // wrapped: 0 is none
        id = m.next++;
    m.entries[id].read = read;
    return id;
}

bool colorReadsWaiting()
{
    ColorMailbox &m = colorMailbox();
    std::lock_guard lock(m.mutex);
    for (const auto &entry : m.entries)
        if (!entry.second.finished)
            return true;
    return false;
}

bool waitColorRead(uint32_t id, std::vector<float> &rgba, uint32_t timeoutMs)
{
    ColorMailbox &m = colorMailbox();
    std::unique_lock lock(m.mutex);
    const bool finished = m.done.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        const auto e = m.entries.find(id);
        return e == m.entries.end() || e->second.finished;
    });
    const auto e = m.entries.find(id);
    if (e == m.entries.end())
        return false;
    const bool ok = finished && e->second.finished && e->second.ok;
    if (ok)
        rgba = std::move(e->second.rgba);
    m.entries.erase(e); // a late answer finds nothing to fill
    return ok;
}

bool readDepth(const DepthBlock &block, std::vector<float> &out)
{
    if (block.target == 0u || block.width == 0u || block.height == 0u ||
        block.width > kMaxDepthSide || block.height > kMaxDepthSide)
        return false;
    DepthMailbox &m = depthMailbox();
    std::lock_guard lock(m.mutex);
    const uint32_t index = m.asked++;
    if (m.wanted.size() < kMaxDepthReads)
        m.wanted.push_back(block);
    if (index >= kMaxDepthReads)
        return false;
    const DepthMailbox::Answer &a = m.answers[index];
    if (!a.valid || a.block.target != block.target || a.block.width != block.width ||
        a.block.height != block.height)
        return false;
    out = a.values;
    return true;
}

namespace
{
struct MeshBufferPool
{
    std::mutex mutex;
    std::vector<std::vector<Vertex>> vertices;
    std::vector<std::vector<uint32_t>> indices;
    std::vector<std::vector<RawVertex>> raw;
    static constexpr size_t kMax = 16384u;
};
MeshBufferPool &meshBufferPool()
{
    static MeshBufferPool pool;
    return pool;
}

// The renderer's: a frame's draws, done with, their buffers to the pool.
void recycleMeshes(std::vector<MeshDraw> &meshes)
{
    if (meshes.empty())
        return;
    MeshBufferPool &pool = meshBufferPool();
    {
        std::lock_guard lock(pool.mutex);
        for (MeshDraw &draw : meshes)
        {
            if (draw.vertices.capacity() != 0u && pool.vertices.size() < MeshBufferPool::kMax)
            {
                draw.vertices.clear();
                pool.vertices.push_back(std::move(draw.vertices));
            }
            if (draw.indices.capacity() != 0u && pool.indices.size() < MeshBufferPool::kMax)
            {
                draw.indices.clear();
                pool.indices.push_back(std::move(draw.indices));
            }
            if (draw.raw.capacity() != 0u && pool.raw.size() < MeshBufferPool::kMax)
            {
                draw.raw.clear();
                pool.raw.push_back(std::move(draw.raw));
            }
        }
    }
    meshes.clear();
}
} // namespace

namespace
{
void reuseBuffers(std::vector<Vertex> &drawVertices, std::vector<uint32_t> &drawIndices)
{
    // A batch at a time, so the pool's lock is taken once per many draws.
    constexpr size_t kBatch = 256u;
    thread_local std::vector<std::vector<Vertex>> vertices;
    thread_local std::vector<std::vector<uint32_t>> indices;
    if (vertices.empty() || indices.empty())
    {
        MeshBufferPool &pool = meshBufferPool();
        std::lock_guard lock(pool.mutex);
        while (vertices.size() < kBatch && !pool.vertices.empty())
        {
            vertices.push_back(std::move(pool.vertices.back()));
            pool.vertices.pop_back();
        }
        while (indices.size() < kBatch && !pool.indices.empty())
        {
            indices.push_back(std::move(pool.indices.back()));
            pool.indices.pop_back();
        }
    }
    if (!vertices.empty() && drawVertices.capacity() == 0u)
    {
        drawVertices = std::move(vertices.back());
        vertices.pop_back();
    }
    if (!indices.empty() && drawIndices.capacity() == 0u)
    {
        drawIndices = std::move(indices.back());
        indices.pop_back();
    }
}
} // namespace

void reuseMeshBuffers(MeshDraw &draw)
{
    reuseBuffers(draw.vertices, draw.indices);
}

void reuseMeshBuffers(Draw2D &draw)
{
    reuseBuffers(draw.vertices, draw.indices);
}

// MeshDraw::raw's vectors, for a producer that fills raw rather than
// vertices: from the same pool, apart from the others (a 2D draw takes none).
void reuseRawBuffer(MeshDraw &draw)
{
    constexpr size_t kBatch = 256u;
    thread_local std::vector<std::vector<RawVertex>> raw;
    if (raw.empty())
    {
        MeshBufferPool &pool = meshBufferPool();
        std::lock_guard lock(pool.mutex);
        while (raw.size() < kBatch && !pool.raw.empty())
        {
            raw.push_back(std::move(pool.raw.back()));
            pool.raw.pop_back();
        }
    }
    if (!raw.empty() && draw.raw.capacity() == 0u)
    {
        draw.raw = std::move(raw.back());
        raw.pop_back();
    }
}

namespace
{
std::atomic<void (*)(MeshDraw &)> g_meshCull{nullptr};
}
void setMeshCull(void (*cull)(MeshDraw &draw))
{
    g_meshCull.store(cull, std::memory_order_release);
}
void (*meshCull())(MeshDraw &draw)
{
    return g_meshCull.load(std::memory_order_acquire);
}

void push(Record &&record)
{
    Queue &q = queue();
    std::lock_guard lock(q.mutex);
    q.records.push_back(std::move(record));
}

std::atomic<int> g_blockFormats{0};

int blockFormats()
{
    return g_blockFormats.load(std::memory_order_relaxed);
}

std::atomic<uint64_t> g_retainedEpoch{1u};
uint64_t retainedEpoch()
{
    return g_retainedEpoch.load(std::memory_order_relaxed);
}
void retainedReset()
{
    g_retainedEpoch.fetch_add(1u, std::memory_order_relaxed);
}

// A defining draw: its triangles into the buffer's pending part, under its
// id. Taken at the record, not at the frame: a frame may be dropped, its
// definitions may not.
void Renderer::retain(const RetainedMesh &d)
{
    if (!d.model.empty())
    {
        // A model chunk (M5): its triangles, each vertex with the three
        // corners' positions, bones and weights, as the static ones below.
        const uint32_t count = static_cast<uint32_t>(d.model.size());
        const uint32_t triangles = d.list ? count / 3u : (count >= 2u ? count - 2u : 0u);
        if (m_modelUsed + triangles * 3u > kModelVertices)
        {
            m_retained.clear();
            m_staticPending.clear();
            m_staticUsed = m_staticUploaded = 0u;
            m_modelPending.clear();
            m_modelUsed = m_modelUploaded = 0u;
            ++m_retainedResets;
            g_retainedEpoch.fetch_add(1u, std::memory_order_relaxed);
        }
        if (triangles == 0u || triangles * 3u > kModelVertices)
            return;
        const size_t at = m_modelPending.size();
        m_modelPending.resize(at + size_t(triangles) * 3u * sizeof(ModelVertex));
        auto *out = reinterpret_cast<ModelVertex *>(m_modelPending.data() + at);
        for (uint32_t k = 2u; k < count; ++k)
        {
            if (d.list && k % 3u != 2u)
                continue;
            const float parity = d.list || (k - 1u) % 2u == 0u ? 1.0f : -1.0f;
            const ModelInput *corner[3] = {&d.model[k - 2u], &d.model[k - 1u], &d.model[k]};
            for (int i = 0; i < 3; ++i, ++out)
            {
                for (int c = 0; c < 3; ++c)
                {
                    std::memcpy(out->position[c], corner[c]->position, sizeof(out->position[c]));
                    std::memcpy(out->bones[c], corner[c]->bones, sizeof(out->bones[c]));
                    std::memcpy(out->weights[c], corner[c]->weights, sizeof(out->weights[c]));
                }
                std::memcpy(out->normal, corner[i]->normal, sizeof(out->normal));
                std::memcpy(out->uv, corner[i]->uv, sizeof(out->uv));
                std::memcpy(out->color, corner[i]->color, sizeof(out->color));
                out->corner = float(i);
                out->parity = parity;
            }
        }
        m_retained[d.id] = {m_modelUsed, triangles * 3u, true};
        m_modelUsed += triangles * 3u;
        ++m_modelDefined;
        return;
    }
    const uint32_t count = static_cast<uint32_t>(d.raw.size());
    const uint32_t triangles = d.list ? count / 3u : (count >= 2u ? count - 2u : 0u);
    if (m_staticUsed + triangles * 3u > kStaticVertices)
    {
        // Full: everything goes, and the producer starts again.
        m_retained.clear();
        m_staticPending.clear();
        m_staticUsed = m_staticUploaded = 0u;
        m_modelPending.clear();
        m_modelUsed = m_modelUploaded = 0u;
        ++m_retainedResets;
        g_retainedEpoch.fetch_add(1u, std::memory_order_relaxed);
    }
    if (triangles * 3u <= kStaticVertices)
    {
        const size_t at = m_staticPending.size();
        m_staticPending.resize(at + size_t(triangles) * 3u * sizeof(StaticVertex));
        auto *out = reinterpret_cast<StaticVertex *>(m_staticPending.data() + at);
        for (uint32_t k = 2u; k < count; ++k)
        {
            if (d.list && k % 3u != 2u)
                continue;
            // The sign the strip's winding has at this triangle (cullMesh:
            // it starts at the second vertex and turns at each); a list's
            // never turns.
            const float parity = d.list || (k - 1u) % 2u == 0u ? 1.0f : -1.0f;
            const RawVertex *corner[3] = {&d.raw[k - 2u], &d.raw[k - 1u], &d.raw[k]};
            for (int i = 0; i < 3; ++i, ++out)
            {
                for (int j = 0; j < 3; ++j)
                {
                    out->a[j] = (&corner[0]->x)[j];
                    out->b[j] = (&corner[1]->x)[j];
                    out->c[j] = (&corner[2]->x)[j];
                }
                out->uv[0] = corner[i]->u;
                out->uv[1] = corner[i]->v;
                out->color[0] = corner[i]->r;
                out->color[1] = corner[i]->g;
                out->color[2] = corner[i]->b;
                out->color[3] = corner[i]->a;
                out->corner = float(i);
                out->parity = parity;
            }
        }
        m_retained[d.id] = {m_staticUsed, triangles * 3u};
        m_staticUsed += triangles * 3u;
        ++m_retainedDefined;
    }
}

std::vector<Id> takeEvicted()
{
    Queue &q = queue();
    std::lock_guard lock(q.mutex);
    std::vector<Id> out;
    out.swap(q.evicted);
    return out;
}

void setProducerStats(std::function<std::string()> line)
{
    Queue &q = queue();
    std::lock_guard lock(q.mutex);
    q.producerStats = std::move(line);
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
        t != m_targets.end() && t->second.role == TargetRole::SceneColor && draw.hasModel && draw.camera != 0u)
        ++r.sceneFloatDepth; // a camera's depth is float, reversed
    if (draw.material.known)
        ++r.materialKnown;
    if (draw.hasNormals)
        ++r.normals;
}

SDL_GPUTexture *Renderer::createTexture(SDL_GPUDevice *device, SDL_GPUTextureFormat format, uint32_t width,
                                        uint32_t height, std::string &error)
{
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(device, &info);
    if (!texture)
        error = std::string("SDL_CreateGPUTexture(native texture): ") + SDL_GetError();
    return texture;
}

void Renderer::apply(SDL_GPUDevice *device, Record &record, std::string &error)
{
    ++m_records[record.index()];
    std::visit(
        [&](auto &r) {
            using T = std::decay_t<decltype(r)>;
            if constexpr (std::is_same_v<T, Target>)
            {
                m_targets[r.id] = r;
                if (r.role == TargetRole::SceneColor)
                    m_sceneTarget = r.id;
            }
            else if constexpr (std::is_same_v<T, Camera>)
            {
                m_cameras[r.id] = r;
                // A moving camera is a new one every frame: keep the window
                // the producer can still name, not the whole session.
                m_newestCamera = std::max(m_newestCamera, r.id);
                if (m_cameras.size() > 4u * kCameraWindow && m_newestCamera > 2u * kCameraWindow)
                    std::erase_if(m_cameras, [&](const auto &c) { return c.first < m_newestCamera - 2u * kCameraWindow; });
            }
            else if constexpr (std::is_same_v<T, MeshDraw>)
            {
                count(r);
                // Any target (step 5a); one not declared is counted when drawn.
                m_meshes.push_back(std::move(r));
            }
            else if constexpr (std::is_same_v<T, Draw2D>)
            {
                // Drawn as a mesh with no model: its positions are the
                // target's pixels.
                MeshDraw draw;
                reuseMeshBuffers(draw);
                draw.target = r.target;
                draw.depthTarget = r.depthTarget;
                draw.camera = r.camera;
                draw.hasModel = false;
                draw.material = r.material;
                draw.scissor = r.scissor;
                draw.vertices.swap(r.vertices);
                draw.indices.swap(r.indices);
                std::copy_n(r.fogColor, 3, draw.fogColor);
                draw.source = r.source;
                count(draw);
                m_meshes.push_back(std::move(draw));
            }
            else if constexpr (std::is_same_v<T, Clear>)
            {
                m_clears.emplace_back(m_meshes.size(), r);
            }
            else if constexpr (std::is_same_v<T, Pass>)
            {
                ++(expandPass(r) ? m_passesDrawn : m_passesRefused);
            }
            else if constexpr (std::is_same_v<T, Present>)
            {
                // The draws since the last one are a frame; a frame not yet
                // drawn when the next closes is dropped (the newest shows).
                m_frameMeshes.swap(m_meshes);
                recycleMeshes(m_meshes);
                m_frameClears.swap(m_clears);
                m_clears.clear();
                m_presentTarget = r.target;
                m_presentRect = r.rect;
                m_presentScale = r.scale;
                m_frameReady = true;
                ++m_meshCounts.frames;
            }
            else if constexpr (std::is_same_v<T, Texture>)
            {
                // A copy of a kept target: at its resolution, read at the
                // game's size, in the target's units.
                if (r.fromTarget != 0u && r.id != 0u)
                {
                    const auto kept = m_gpuTargets.find(r.fromTarget);
                    if (kept != m_gpuTargets.end() && kept->second.texture && kept->second.width != 0u)
                    {
                        const uint32_t s = std::max<uint32_t>(m_frameScale, 1u);
                        const uint32_t w = std::min<uint32_t>(kept->second.width, uint32_t(r.width) * s);
                        const uint32_t h = std::min<uint32_t>(kept->second.height, uint32_t(r.height) * s);
                        SDL_GPUTextureCreateInfo info{};
                        info.type = SDL_GPU_TEXTURETYPE_2D;
                        info.format = kColorFormat;
                        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
                        info.width = w;
                        info.height = h;
                        info.layer_count_or_depth = 1;
                        info.num_levels = 1;
                        if (SDL_GPUTexture *texture = SDL_CreateGPUTexture(device, &info))
                        {
                            if (const auto old = m_images.find(r.id); old != m_images.end())
                            {
                                SDL_ReleaseGPUTexture(device, old->second.texture);
                                m_imageBytes -= old->second.bytes;
                                m_imageOrder.erase(std::find(m_imageOrder.begin(), m_imageOrder.end(), r.id));
                            }
                            const uint64_t bytes = uint64_t(w) * h * 8u;
                            m_images[r.id] = Image{texture, PixelFormat::Rgba8, w, h, 0u, bytes, r.width, r.height, 1.0f};
                            m_imageOrder.push_back(r.id);
                            m_imageBytes += bytes;
                            m_targetCopies.push_back({kept->second.texture, texture, w, h});
                            ++m_keptTextures;
                            return;
                        }
                    }
                }
                // Bytes a texel, or a 4x4 block for the block formats.
                SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
                uint64_t bytes = 0u;
                const uint64_t blocks = uint64_t((r.width + 3u) / 4u) * ((r.height + 3u) / 4u);
                switch (r.format)
                {
                case PixelFormat::Rgba8:
                    format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
                    bytes = uint64_t(r.width) * r.height * 4u;
                    break;
                case PixelFormat::Index8:
                    format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
                    bytes = uint64_t(r.width) * r.height;
                    break;
                case PixelFormat::Ct16:
                    format = ct16Format(device);
                    bytes = uint64_t(r.width) * r.height * 2u;
                    break;
                case PixelFormat::Index4: // two texels a byte: a texture half as wide
                    format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
                    bytes = (r.width & 1u) ? 0u : uint64_t(r.width / 2u) * r.height;
                    break;
                case PixelFormat::Bc1:
                    format = SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
                    bytes = blocks * 8u;
                    break;
                case PixelFormat::Bc2:
                    format = SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM;
                    bytes = blocks * 16u;
                    break;
                case PixelFormat::Bc3:
                    format = SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
                    bytes = blocks * 16u;
                    break;
                case PixelFormat::Bc7:
                    format = SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM;
                    bytes = blocks * 16u;
                    break;
                default:
                    break;
                }
                if (r.id == 0u || format == SDL_GPU_TEXTUREFORMAT_INVALID || bytes == 0u || !r.texels ||
                    r.texels->size() < bytes)
                {
                    ++m_refused;
                    return;
                }
                const uint32_t storedWidth = r.format == PixelFormat::Index4 ? r.width / 2u : r.width;
                SDL_GPUTexture *texture = createTexture(device, format, storedWidth, r.height, error);
                if (!texture)
                    return;
                if (const auto old = m_images.find(r.id); old != m_images.end())
                {
                    SDL_ReleaseGPUTexture(device, old->second.texture);
                    m_imageBytes -= old->second.bytes;
                    m_imageOrder.erase(std::find(m_imageOrder.begin(), m_imageOrder.end(), r.id));
                }
                m_images[r.id] = Image{texture, r.format, r.width, r.height, r.palette, bytes, r.logicalWidth,
                                       r.logicalHeight, r.scale};
                m_imageOrder.push_back(r.id);
                m_imageBytes += bytes;
                m_uploads.push_back({texture, storedWidth, r.height,
                                     r.format == PixelFormat::Ct16 ? ct16Texels(device, r.texels, bytes / 2u) : r.texels});
                m_recent.erase(std::remove(m_recent.begin(), m_recent.end(), r.id), m_recent.end());
                m_recent.push_front(r.id);
                if (m_recent.size() > kViewColumns * kViewRows)
                    m_recent.pop_back();
            }
            else if constexpr (std::is_same_v<T, RetainedMesh>)
            {
                // At the record, not at the frame: a frame may be dropped,
                // its definitions may not.
                if (r.id != 0u)
                    retain(r);
            }
            else if constexpr (std::is_same_v<T, Palette>)
            {
                const bool ct16 = r.format == PixelFormat::Ct16;
                const uint32_t entryBytes = ct16 ? 2u : 4u;
                if (r.id == 0u || (r.entries != 16u && r.entries != 256u) || !r.colors ||
                    r.colors->size() < r.entries * entryBytes)
                {
                    ++m_refused;
                    return;
                }
                SDL_GPUTexture *texture = createTexture(
                    device, ct16 ? ct16Format(device) : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, r.entries, 1u, error);
                if (!texture)
                    return;
                if (const auto old = m_palettes.find(r.id); old != m_palettes.end())
                    SDL_ReleaseGPUTexture(device, old->second.texture);
                m_palettes[r.id] =
                    Image{texture, r.format, r.entries, 1u, 0u, r.entries * entryBytes, 0u, 0u, r.scale};
                m_uploads.push_back({texture, r.entries, 1u, ct16 ? ct16Texels(device, r.colors, r.entries) : r.colors});
            }
            // The rest is drawn from step 4 on; counted above.
        },
        record);
}

SDL_GPUTextureFormat Renderer::ct16Format(SDL_GPUDevice *device)
{
    if (m_ct16Native < 0)
        m_ct16Native = SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_B5G5R5A1_UNORM,
                                                    SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER)
                           ? 1
                           : 0;
    return m_ct16Native ? SDL_GPU_TEXTUREFORMAT_B5G5R5A1_UNORM : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
}

Bytes Renderer::ct16Texels(SDL_GPUDevice *device, const Bytes &raw, size_t count)
{
    // B5G5R5A1 holds the GS's texels as they are: its blue reads the GS's
    // red (bits 0-4). Without it, the same channels as RGBA8 (blue, green,
    // red, alpha: what B5G5R5A1 would read), so the shader is one.
    if (ct16Format(device) == SDL_GPU_TEXTUREFORMAT_B5G5R5A1_UNORM || !raw)
        return raw;
    auto out = std::make_shared<std::vector<uint8_t>>(count * 4u);
    const auto widen = [](uint32_t v) { return uint8_t(((v & 31u) * 255u + 15u) / 31u); };
    for (size_t i = 0; i < count && i * 2u + 1u < raw->size(); ++i)
    {
        const uint32_t v = (*raw)[i * 2u] | ((*raw)[i * 2u + 1u] << 8);
        uint8_t *o = out->data() + i * 4u;
        o[0] = widen(v >> 10);
        o[1] = widen(v >> 5);
        o[2] = widen(v);
        o[3] = (v & 0x8000u) ? 255u : 0u;
    }
    return out;
}

bool Renderer::upload(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, std::string &error)
{
    for (const PendingCopy &c : m_targetCopies)
    {
        SDL_GPUBlitInfo blit{};
        blit.source.texture = c.from;
        blit.source.w = c.width;
        blit.source.h = c.height;
        blit.destination.texture = c.to;
        blit.destination.w = c.width;
        blit.destination.h = c.height;
        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(commands, &blit);
    }
    m_targetCopies.clear();
    if (m_uploads.empty())
        return true;
    // Each upload at a multiple of 16 bytes: a block format's offset has to
    // be a whole number of its blocks.
    const auto aligned = [](uint64_t n) { return (n + 15u) & ~uint64_t(15u); };
    uint64_t total = 0u;
    for (const PendingUpload &u : m_uploads)
        total += aligned(u.bytes->size());
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = static_cast<uint32_t>(total);
    SDL_GPUTransferBuffer *buffer = SDL_CreateGPUTransferBuffer(device, &info);
    if (!buffer)
    {
        error = std::string("SDL_CreateGPUTransferBuffer(native textures): ") + SDL_GetError();
        return false;
    }
    auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(device, buffer, false));
    if (!mapped)
    {
        error = std::string("SDL_MapGPUTransferBuffer(native textures): ") + SDL_GetError();
        SDL_ReleaseGPUTransferBuffer(device, buffer);
        return false;
    }
    uint32_t offset = 0u;
    for (const PendingUpload &u : m_uploads)
    {
        std::memcpy(mapped + offset, u.bytes->data(), u.bytes->size());
        offset += static_cast<uint32_t>(aligned(u.bytes->size()));
    }
    SDL_UnmapGPUTransferBuffer(device, buffer);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    offset = 0u;
    for (const PendingUpload &u : m_uploads)
    {
        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = buffer;
        source.offset = offset;
        source.pixels_per_row = u.width;
        source.rows_per_layer = u.height;
        SDL_GPUTextureRegion destination{};
        destination.texture = u.texture;
        destination.w = u.width;
        destination.h = u.height;
        destination.d = 1u;
        SDL_UploadToGPUTexture(copy, &source, &destination, false);
        offset += static_cast<uint32_t>(aligned(u.bytes->size()));
    }
    SDL_EndGPUCopyPass(copy);
    // Freed once the GPU is done with it.
    SDL_ReleaseGPUTransferBuffer(device, buffer);
    m_uploads.clear();
    return true;
}

void Renderer::evict(SDL_GPUDevice *device)
{
    std::vector<Id> gone;
    while (m_imageBytes > kImageBudget && !m_imageOrder.empty())
    {
        const Id id = m_imageOrder.front();
        m_imageOrder.pop_front();
        const auto it = m_images.find(id);
        if (it == m_images.end())
            continue;
        SDL_ReleaseGPUTexture(device, it->second.texture);
        m_imageBytes -= it->second.bytes;
        m_images.erase(it);
        m_recent.erase(std::remove(m_recent.begin(), m_recent.end(), id), m_recent.end());
        gone.push_back(id);
        ++m_evicted;
    }
    if (gone.empty())
        return;
    Queue &q = queue();
    std::lock_guard lock(q.mutex);
    q.evicted.insert(q.evicted.end(), gone.begin(), gone.end());
}

bool Renderer::ensureView(SDL_GPUDevice *device, std::string &error)
{
    if (m_viewPipeline)
        return true;
    if (m_viewFailed)
        return false;
    m_viewFailed = true; // until it is made
    // SPIR-V only so far (no MSL build of these shaders): no view elsewhere.
    if (!(SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_SPIRV))
        return false;
    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    m_viewSampler = SDL_CreateGPUSampler(device, &samplerInfo);
    if (!m_viewSampler)
    {
        error = std::string("SDL_CreateGPUSampler(native view): ") + SDL_GetError();
        return false;
    }
    SDL_GPUShaderCreateInfo vertexInfo{};
    vertexInfo.code = reinterpret_cast<const uint8_t *>(kNativeViewVertSpirv);
    vertexInfo.code_size = sizeof(kNativeViewVertSpirv);
    vertexInfo.entrypoint = "main";
    vertexInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vertexInfo.num_uniform_buffers = 1u;
    SDL_GPUShaderCreateInfo fragmentInfo{};
    fragmentInfo.code = reinterpret_cast<const uint8_t *>(kNativeViewFragSpirv);
    fragmentInfo.code_size = sizeof(kNativeViewFragSpirv);
    fragmentInfo.entrypoint = "main";
    fragmentInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fragmentInfo.num_samplers = 2u;
    fragmentInfo.num_uniform_buffers = 1u;
    SDL_GPUShader *vertex = SDL_CreateGPUShader(device, &vertexInfo);
    SDL_GPUShader *fragment = vertex ? SDL_CreateGPUShader(device, &fragmentInfo) : nullptr;
    if (!vertex || !fragment)
    {
        error = std::string("SDL_CreateGPUShader(native view): ") + SDL_GetError();
        if (vertex)
            SDL_ReleaseGPUShader(device, vertex);
        return false;
    }
    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = kColorFormat;
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    info.target_info.color_target_descriptions = &colorTarget;
    info.target_info.num_color_targets = 1u;
    m_viewPipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    SDL_ReleaseGPUShader(device, vertex);
    SDL_ReleaseGPUShader(device, fragment);
    if (!m_viewPipeline)
    {
        error = std::string("SDL_CreateGPUGraphicsPipeline(native view): ") + SDL_GetError();
        return false;
    }
    m_viewFailed = false;
    return true;
}

// The newest textures, newest first, each fitted into a cell of the grid.
void Renderer::drawView(SDL_GPUCommandBuffer *commands, SDL_GPURenderPass *pass)
{
    SDL_BindGPUGraphicsPipeline(pass, m_viewPipeline);
    const float cellW = 2.0f / kViewColumns, cellH = 2.0f / kViewRows;
    const float aspect = float(m_sceneWidth) / float(std::max(m_sceneHeight, 1u));
    uint32_t cell = 0u;
    for (const Id id : m_recent)
    {
        const auto it = m_images.find(id);
        if (it == m_images.end())
            continue;
        const Image &image = it->second;
        uint32_t mode[4] = {0u, 0u, 0u, 0u};
        SDL_GPUTexture *palette = image.texture;
        if (image.format == PixelFormat::Ct16)
            mode[0] = 3u;
        mode[2] = image.format == PixelFormat::Index4 ? 1u : 0u;
        if (image.format == PixelFormat::Index8 || image.format == PixelFormat::Index4)
        {
            const auto p = m_palettes.find(image.palette);
            if (p != m_palettes.end())
            {
                mode[0] = p->second.format == PixelFormat::Ct16 ? 4u : 1u;
                mode[1] = p->second.width;
                palette = p->second.texture;
            }
            else
                mode[0] = 2u;
        }
        // The cell, less a margin, then the texture's shape inside it (in
        // clip space, whose x and y units differ by the scene's aspect).
        const float x0 = -1.0f + (cell % kViewColumns) * cellW, top = 1.0f - (cell / kViewColumns) * cellH;
        const float margin = 0.08f;
        float w = cellW * (1.0f - margin), h = cellH * (1.0f - margin);
        const float shape = float(image.width) / float(std::max(image.height, 1u)); // width over height
        if (shape * h / aspect > w)
            h = w * aspect / shape;
        else
            w = shape * h / aspect;
        const float cx = x0 + cellW * 0.5f, cy = top - cellH * 0.5f;
        const float rect[4] = {cx - w * 0.5f, cy + h * 0.5f, cx + w * 0.5f, cy - h * 0.5f};
        SDL_PushGPUVertexUniformData(commands, 0, rect, sizeof(rect));
        SDL_PushGPUFragmentUniformData(commands, 0, mode, sizeof(mode));
        const SDL_GPUTextureSamplerBinding bindings[2] = {{image.texture, m_viewSampler}, {palette, m_viewSampler}};
        SDL_BindGPUFragmentSamplers(pass, 0, bindings, 2);
        SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
        if (++cell == kViewColumns * kViewRows)
            break;
    }
}

bool Renderer::render(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, uint32_t scale, Frame &out,
                      std::string &error)
{
    // Once: the block formats a texture pack's pictures may keep.
    static const bool formatsKnown = [device] {
        int formats = 0;
        const std::pair<SDL_GPUTextureFormat, int> known[] = {{SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM, 1},
                                                              {SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM, 2},
                                                              {SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM, 4},
                                                              {SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM, 8}};
        for (const auto &[format, bit] : known)
            if (SDL_GPUTextureSupportsFormat(device, format, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER))
                formats |= bit;
        g_blockFormats.store(formats, std::memory_order_relaxed);
        return true;
    }();
    (void)formatsKnown;
    {
        Queue &q = queue();
        std::lock_guard lock(q.mutex);
        m_drained.swap(q.records);
    }
    // A texture the GPU refuses is counted and left out, not the frame lost.
    static bool told = false;
    for (Record &record : m_drained)
    {
        std::string refused;
        apply(device, record, refused);
        if (!refused.empty())
        {
            ++m_refused;
            if (!told)
                std::fprintf(stderr, "[native-pipeline] refused: %s\n", refused.c_str());
            told = true;
        }
    }
    m_drained.clear();
    std::string meshError;
    const bool meshes = ensureMeshes(device, meshError);
    static bool toldMeshes = false;
    if (!meshes && !toldMeshes)
    {
        std::fprintf(stderr, "[native-pipeline] no mesh draws: %s\n", meshError.c_str());
        toldMeshes = true;
    }
    if (std::string failed; !upload(device, commands, failed))
    {
        std::fprintf(stderr, "[native-pipeline] uploads lost: %s\n", failed.c_str());
        m_uploads.clear();
    }
    evict(device);
    // The depth reads, from the last frame drawn (submitted ahead of this
    // one's draws).
    serveDepth(device);

    uint32_t width = kDefaultWidth, height = kDefaultHeight;
    if (const auto t = m_targets.find(m_sceneTarget); t != m_targets.end() && t->second.width && t->second.height)
    {
        width = t->second.width;
        height = t->second.height;
    }
    scale = scale ? scale : 1u;
    width *= scale;
    height *= scale;
    bool fresh = false; // the scene target holds nothing yet
    if (!m_scene || m_sceneWidth != width || m_sceneHeight != height)
    {
        fresh = true;
        if (m_scene)
            SDL_ReleaseGPUTexture(device, m_scene);
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = kColorFormat;
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

    // The scene: the meshes of the last frame a Present record closed, over
    // black, with a reversed depth target of its own. Kept as it is until
    // the next frame closes (the host presents more often than the game).
    static const bool grid = [] {
        const char *v = std::getenv("RG_NATIVE_TEXTURE_GRID");
        return v && *v && *v != '0';
    }();
    // What the Present record shows: its target (the scene unless it names
    // another), the rectangle it names (all of it when none), resolved into
    // the 8-bit picture at its scale when it changed.
    const auto shown = [&](bool changed) {
        SDL_GPUTexture *source = m_scene;
        uint32_t x = 0u, y = 0u, w = m_sceneWidth, h = m_sceneHeight;
        if (m_presentTarget != 0u && m_presentTarget != m_sceneTarget)
        {
            const auto t = m_gpuTargets.find(m_presentTarget);
            if (t != m_gpuTargets.end() && t->second.texture)
            {
                source = t->second.texture;
                w = t->second.width;
                h = t->second.height;
            }
        }
        const Rect &r = m_presentRect;
        if (r.x1 > r.x0 && r.y1 > r.y0 && r.x0 >= 0 && r.y0 >= 0)
        {
            x = std::min(uint32_t(r.x0) * scale, w);
            y = std::min(uint32_t(r.y0) * scale, h);
            w = std::min(uint32_t(r.x1 - r.x0) * scale, w - x);
            h = std::min(uint32_t(r.y1 - r.y0) * scale, h - y);
        }
        if ((changed || !m_output) && !resolve(device, commands, source, x, y, w, h, error))
            return false;
        out.texture = m_output;
        out.x = out.y = 0u;
        out.width = m_outputWidth;
        out.height = m_outputHeight;
        return true;
    };
    const bool draw = meshes && m_frameReady;
    if (!draw && !fresh && !grid)
        return shown(false);
    bool drawn = false;
    if (draw)
    {
        std::string failed;
        drawn = drawFrame(device, commands, scale, failed);
        if (drawn)
            m_frameScale = scale;
        if (!drawn)
            std::fprintf(stderr, "[native-pipeline] frame lost: %s\n", failed.c_str());
    }
    m_frameReady = false;
    std::string viewError;
    const bool view = grid && ensureView(device, viewError);
    if (!viewError.empty())
        std::fprintf(stderr, "[native-pipeline] no texture view: %s\n", viewError.c_str());
    if (!drawn || view)
    {
        // A new scene target with no frame for it yet is cleared; the grid
        // goes over whatever the scene holds.
        SDL_GPUColorTargetInfo target{};
        target.texture = m_scene;
        target.load_op = drawn ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        target.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr);
        if (!pass)
        {
            error = std::string("SDL_BeginGPURenderPass(native scene): ") + SDL_GetError();
            return false;
        }
        if (view)
            drawView(commands, pass);
        SDL_EndGPURenderPass(pass);
    }
    return shown(true);
}

bool Renderer::resolve(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, SDL_GPUTexture *source, uint32_t x,
                       uint32_t y, uint32_t width, uint32_t height, std::string &error)
{
    if (m_resolveFailed)
    {
        error = "no resolve pipeline";
        return false;
    }
    if (!m_resolvePipeline)
    {
        m_resolveFailed = true; // until made
        SDL_GPUShaderCreateInfo vertexInfo{};
        vertexInfo.code = reinterpret_cast<const uint8_t *>(kNativeResolveVertSpirv);
        vertexInfo.code_size = sizeof(kNativeResolveVertSpirv);
        vertexInfo.entrypoint = "main";
        vertexInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
        vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        SDL_GPUShaderCreateInfo fragmentInfo{};
        fragmentInfo.code = reinterpret_cast<const uint8_t *>(kNativeResolveFragSpirv);
        fragmentInfo.code_size = sizeof(kNativeResolveFragSpirv);
        fragmentInfo.entrypoint = "main";
        fragmentInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
        fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        fragmentInfo.num_samplers = 1u;
        fragmentInfo.num_uniform_buffers = 1u;
        SDL_GPUShader *vertex = SDL_CreateGPUShader(device, &vertexInfo);
        SDL_GPUShader *fragment = vertex ? SDL_CreateGPUShader(device, &fragmentInfo) : nullptr;
        if (fragment)
        {
            SDL_GPUColorTargetDescription color{};
            color.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = vertex;
            info.fragment_shader = fragment;
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            info.target_info.color_target_descriptions = &color;
            info.target_info.num_color_targets = 1u;
            m_resolvePipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
        }
        if (fragment)
            SDL_ReleaseGPUShader(device, fragment);
        if (vertex)
            SDL_ReleaseGPUShader(device, vertex);
        SDL_GPUSamplerCreateInfo samplerInfo{};
        m_resolveSampler = m_resolvePipeline ? SDL_CreateGPUSampler(device, &samplerInfo) : nullptr;
        if (!m_resolvePipeline || !m_resolveSampler)
        {
            error = std::string("the native pipeline's resolve: ") + SDL_GetError();
            return false;
        }
        m_resolveFailed = false;
    }
    if (width == 0u || height == 0u || !source)
    {
        error = "nothing to show";
        return false;
    }
    if (!m_output || m_outputWidth != width || m_outputHeight != height)
    {
        if (m_output)
            SDL_ReleaseGPUTexture(device, m_output);
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        m_output = SDL_CreateGPUTexture(device, &info);
        m_outputWidth = m_output ? width : 0u;
        m_outputHeight = m_output ? height : 0u;
        if (!m_output)
        {
            error = std::string("SDL_CreateGPUTexture(native output): ") + SDL_GetError();
            return false;
        }
    }
    SDL_GPUColorTargetInfo target{};
    target.texture = m_output;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr);
    if (!pass)
    {
        error = std::string("SDL_BeginGPURenderPass(native resolve): ") + SDL_GetError();
        return false;
    }
    SDL_BindGPUGraphicsPipeline(pass, m_resolvePipeline);
    const SDL_GPUTextureSamplerBinding binding{source, m_resolveSampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    struct
    {
        int32_t origin[4];
        float scale[4];
    } params{{int32_t(x), int32_t(y), 0, 0}, {m_presentScale, 0.0f, 0.0f, 0.0f}};
    SDL_PushGPUFragmentUniformData(commands, 0, &params, sizeof(params));
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    return true;
}

Renderer::Totals Renderer::totals() const
{
    Totals t;
    t.frames = m_meshCounts.frames;
    t.meshesDrawn = m_meshCounts.drawn;
    t.gpuCalls = m_meshCounts.calls;
    t.passes = m_meshCounts.passes;
    t.copies = m_meshCounts.copies;
    t.texturesHeld = m_images.size() + m_palettes.size();
    t.textureBytes = m_imageBytes;
    t.retainedMeshes = m_retained.size();
    return t;
}

std::string Renderer::statsLine()
{
    std::string line = "[native-pipeline] records:";
    for (size_t i = 0; i < std::size(kRecordNames); ++i)
    {
        line += std::string(" ") + kRecordNames[i] + " " + std::to_string(m_records[i] - m_reported[i]);
        m_reported[i] = m_records[i];
    }
    const auto d = [](uint64_t a, uint64_t b) { return std::to_string(a - b); };
    line += " | held: textures " + std::to_string(m_images.size()) + " (" + std::to_string(m_imageBytes >> 20) +
            " MiB), palettes " + std::to_string(m_palettes.size()) + ", evicted " + d(m_evicted, m_evictedReported) +
            ", refused " + d(m_refused, m_refusedReported) + " | retained meshes " +
            std::to_string(m_retained.size()) + " (" + std::to_string(m_staticUsed) + " vertices, defined so far " +
            std::to_string(m_retainedDefined) + ", draws so far " + std::to_string(m_retainedDrawn) +
            ", of an id not held " + std::to_string(m_retainedMissing) + ", resets " +
            std::to_string(m_retainedResets) + ") | model chunks: defined so far " + std::to_string(m_modelDefined) +
            " (" + std::to_string(m_modelUsed) + " vertices), draws " + d(m_modelDrawn, m_modelDrawnReported) +
            " | clears of part of a target (not drawn, so far) " +
            std::to_string(m_clearsRefused) + " | passes drawn (so far) " + std::to_string(m_passesDrawn) +
            ", refused " + std::to_string(m_passesRefused);
    m_evictedReported = m_evicted;
    m_refusedReported = m_refused;
    m_modelDrawnReported = m_modelDrawn;
    const Readiness &now = m_readiness, &was = m_readinessReported;
    line += " | readiness of " + d(now.meshDraws, was.meshDraws) + " mesh draws: R1 camera+model " +
            d(now.cameraAndModel, was.cameraAndModel) + ", R2 object " + d(now.objectIdentity, was.objectIdentity) +
            ", R3 scene float depth " + d(now.sceneFloatDepth, was.sceneFloatDepth) + ", R4 material " +
            d(now.materialKnown, was.materialKnown) + ", R5 normals " + d(now.normals, was.normals);
    const MeshCounts &mc = m_meshCounts, &mr = m_meshReported;
    line += " | meshes drawn " + d(mc.drawn, mr.drawn) + " in " + d(mc.calls, mr.calls) + " GPU calls (offscreen " +
            d(mc.offscreen, mr.offscreen) + ") over " +
            d(mc.frames, mr.frames) + " frames in " + d(mc.passes, mr.passes) +
            " passes; not drawn: target not declared " + d(mc.noTarget, mr.noTarget) + ", a depth buffer as color " +
            d(mc.depthAsColor, mr.depthAsColor) + ", no camera " + d(mc.noCamera, mr.noCamera) +
            ", texture not held " + d(mc.noTexture, mr.noTexture) + ", no pipeline " +
            d(mc.noPipeline, mr.noPipeline) + ", source target never drawn " +
            d(mc.sourceNotDrawn, mr.sourceNotDrawn) + ", reading its own target uncopied " + d(mc.feedback, mr.feedback) + ", copies for those that do " +
            d(mc.copies, mr.copies) + " (destination alpha tests " + d(mc.destinationAlpha, mr.destinationAlpha) + ")" +
            "; window space " + d(mc.window, mr.window) + ", from a target " + d(mc.fromTarget, mr.fromTarget) +
            "; a second depth target in a pass " +
            d(mc.depthMismatch, mr.depthMismatch) + "\n";
    const DepthCounts &dc = m_depthCounts, &dr = m_depthReported;
    if (dc.asked != dr.asked)
        line += "  [native-pipeline] depth reads: asked " + d(dc.asked, dr.asked) + ", copied " + d(dc.copied, dr.copied) +
                ", published " + d(dc.published, dr.published) + ", no such depth target (the clear's) " +
                d(dc.noTarget, dr.noTarget) + ", left for a copy in flight " + d(dc.waiting, dr.waiting) +
                ", unserved " + d(dc.unserved, dr.unserved) + "\n";
    m_depthReported = m_depthCounts;
    if (m_colorReads != m_colorReadsReported || m_colorReadsFailed != m_colorFailedReported)
        line += "  [native-pipeline] colour reads: served " + d(m_colorReads, m_colorReadsReported) + ", failed " +
                d(m_colorReadsFailed, m_colorFailedReported) + ", textures from kept reads " +
                d(m_keptTextures, m_keptTexturesReported) + "\n";
    m_colorReadsReported = m_colorReads;
    m_colorFailedReported = m_colorReadsFailed;
    m_keptTexturesReported = m_keptTextures;
    m_meshReported = m_meshCounts;
    m_readinessReported = m_readiness;
    std::function<std::string()> producer;
    {
        Queue &q = queue();
        std::lock_guard lock(q.mutex);
        producer = q.producerStats;
    }
    if (producer)
        line += "  " + producer() + "\n";
    return line;
}

void Renderer::serveDepth(SDL_GPUDevice *device)
{
    // The copy the GPU has finished: each block brought to the game's
    // pixels (the nearest of the pixels each covers) and published.
    if (m_depthFence)
    {
        if (!SDL_QueryGPUFence(device, m_depthFence))
        {
            m_depthCounts.waiting += takeDepthReads().size(); // asked again next frame
            return;
        }
        SDL_ReleaseGPUFence(device, m_depthFence);
        m_depthFence = nullptr;
        const auto *mapped = static_cast<const float *>(SDL_MapGPUTransferBuffer(device, m_depthDownload, false));
        if (mapped)
        {
            const uint32_t s = m_depthCopyScale;
            size_t offset = 0u;
            for (const auto &[index, block] : m_depthCopied)
            {
                const uint32_t w = block.width * s, h = block.height * s;
                std::vector<float> values(size_t(block.width) * block.height, 0.0f);
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        float &v = values[size_t(y / s) * block.width + x / s];
                        v = std::max(v, mapped[offset + size_t(y) * w + x]);
                    }
                offset += size_t(w) * h;
                publishDepth(index, block, std::move(values));
                ++m_depthCounts.published;
            }
            SDL_UnmapGPUTransferBuffer(device, m_depthDownload);
        }
        m_depthCopied.clear();
    }

    const std::vector<DepthBlock> wanted = takeDepthReads();
    if (wanted.empty())
        return;
    m_depthCounts.asked += wanted.size();
    // Only a float depth is copied out as it is.
    if (m_depthFailed || m_depthFormat != SDL_GPU_TEXTUREFORMAT_D32_FLOAT || m_frameScale == 0u)
    {
        m_depthCounts.unserved += wanted.size();
        return;
    }
    const uint32_t s = m_frameScale;
    std::vector<std::pair<uint32_t, DepthBlock>> copies;
    size_t floats = 0u;
    for (uint32_t index = 0; index < wanted.size(); ++index)
    {
        const DepthBlock &b = wanted[index];
        const auto t = m_gpuTargets.find(b.target);
        if (t == m_gpuTargets.end() || !t->second.texture || b.x < 0 || b.y < 0 ||
            (uint32_t(b.x) + b.width) * s > t->second.width || (uint32_t(b.y) + b.height) * s > t->second.height)
        {
            // Never drawn here: the clear's depth, as the GS's memory held.
            publishDepth(index, b, std::vector<float>(size_t(b.width) * b.height, 0.0f));
            ++m_depthCounts.noTarget;
            continue;
        }
        copies.emplace_back(index, b);
        floats += size_t(b.width) * s * b.height * s;
    }
    if (copies.empty())
        return;
    const auto fail = [&](const char *what) {
        std::fprintf(stderr, "[native-pipeline] depth reads off: %s: %s\n", what, SDL_GetError());
        m_depthFailed = true;
        m_depthCounts.unserved += copies.size();
    };
    const uint32_t bytes = uint32_t(floats * sizeof(float));
    if (m_depthDownloadBytes < bytes)
    {
        if (m_depthDownload)
            SDL_ReleaseGPUTransferBuffer(device, m_depthDownload);
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        info.size = bytes;
        m_depthDownload = SDL_CreateGPUTransferBuffer(device, &info);
        m_depthDownloadBytes = m_depthDownload ? bytes : 0u;
        if (!m_depthDownload)
            return fail("SDL_CreateGPUTransferBuffer");
    }
    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
    if (!commands)
        return fail("SDL_AcquireGPUCommandBuffer");
    SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(commands);
    if (!pass)
    {
        SDL_CancelGPUCommandBuffer(commands);
        return fail("SDL_BeginGPUCopyPass");
    }
    uint32_t offset = 0u;
    for (const auto &[index, b] : copies)
    {
        SDL_GPUTextureRegion region{};
        region.texture = m_gpuTargets[b.target].texture;
        region.x = uint32_t(b.x) * s;
        region.y = uint32_t(b.y) * s;
        region.w = b.width * s;
        region.h = b.height * s;
        region.d = 1u;
        SDL_GPUTextureTransferInfo destination{};
        destination.transfer_buffer = m_depthDownload;
        destination.offset = offset;
        destination.pixels_per_row = region.w;
        destination.rows_per_layer = region.h;
        SDL_DownloadFromGPUTexture(pass, &region, &destination);
        offset += region.w * region.h * uint32_t(sizeof(float));
    }
    SDL_EndGPUCopyPass(pass);
    m_depthFence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!m_depthFence)
        return fail("SDL_SubmitGPUCommandBufferAndAcquireFence");
    m_depthCopied = std::move(copies);
    m_depthCopyScale = s;
    m_depthCounts.copied += m_depthCopied.size();
}

void Renderer::dumpTargets(SDL_GPUDevice *device)
{
    m_dumpTargets = false;
    std::vector<std::tuple<Id, SDL_GPUTexture *, uint32_t, uint32_t>> all;
    all.emplace_back(m_sceneTarget, m_scene, m_sceneWidth, m_sceneHeight);
    for (const auto &[id, t] : m_gpuTargets)
    {
        const auto declared = m_targets.find(id);
        if (!t.texture || declared == m_targets.end() || declared->second.format == PixelFormat::Depth32 ||
            id == m_sceneTarget)
            continue;
        all.emplace_back(id, t.texture, t.width, t.height);
    }
    std::error_code ec;
    std::filesystem::create_directories("build/run/targets", ec);
    for (const auto &[id, texture, w, h] : all)
    {
        if (!texture || w == 0u || h == 0u)
            continue;
        const uint32_t bytes = w * h * 8u;
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        info.size = bytes;
        SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(device, &info);
        SDL_GPUCommandBuffer *commands = download ? SDL_AcquireGPUCommandBuffer(device) : nullptr;
        SDL_GPUCopyPass *pass = commands ? SDL_BeginGPUCopyPass(commands) : nullptr;
        SDL_GPUFence *fence = nullptr;
        if (pass)
        {
            SDL_GPUTextureRegion region{};
            region.texture = texture;
            region.w = w;
            region.h = h;
            region.d = 1u;
            SDL_GPUTextureTransferInfo destination{};
            destination.transfer_buffer = download;
            destination.pixels_per_row = w;
            destination.rows_per_layer = h;
            SDL_DownloadFromGPUTexture(pass, &region, &destination);
            SDL_EndGPUCopyPass(pass);
            fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        }
        else if (commands)
            SDL_CancelGPUCommandBuffer(commands);
        if (fence)
        {
            SDL_WaitForGPUFences(device, true, &fence, 1u);
            SDL_ReleaseGPUFence(device, fence);
            if (const auto *p = static_cast<const uint16_t *>(SDL_MapGPUTransferBuffer(device, download, false)))
            {
                // As shown: the Present record's scale, clamped.
                const auto shown = [&](uint16_t h) {
                    return uint8_t(std::clamp(halfToFloat(h) * m_presentScale * 255.0f + 0.5f, 0.0f, 255.0f));
                };
                char name[96];
                std::snprintf(name, sizeof(name), "build/run/targets/target%u_%ux%u.ppm", id, w, h);
                if (FILE *f = std::fopen(name, "wb"))
                {
                    std::fprintf(f, "P6\n%u %u\n255\n", w * 2u, h);
                    std::vector<uint8_t> row(size_t(w) * 6u);
                    for (uint32_t y = 0; y < h; ++y)
                    {
                        for (uint32_t x = 0; x < w; ++x)
                        {
                            const uint16_t *px = p + (size_t(y) * w + x) * 4u;
                            for (uint32_t c = 0; c < 3u; ++c)
                                row[x * 3u + c] = shown(px[c]);
                            row[(w + x) * 3u] = row[(w + x) * 3u + 1u] = row[(w + x) * 3u + 2u] = shown(px[3]);
                        }
                        std::fwrite(row.data(), 1, row.size(), f);
                    }
                    std::fclose(f);
                    std::fprintf(stderr, "[target-dump] %s\n", name);
                }
                SDL_UnmapGPUTransferBuffer(device, download);
            }
        }
        if (download)
            SDL_ReleaseGPUTransferBuffer(device, download);
    }
}

void Renderer::serveColorReads(SDL_GPUDevice *device)
{
    if (m_dumpTargets)
        dumpTargets(device);
    std::vector<std::pair<uint32_t, ColorRead>> waiting;
    ColorMailbox &m = colorMailbox();
    {
        std::lock_guard lock(m.mutex);
        for (const auto &[id, e] : m.entries)
            if (!e.finished)
                waiting.emplace_back(id, e.read);
    }
    if (waiting.empty())
        return;
    const auto finish = [&](uint32_t id, std::vector<float> &&rgba, bool ok) {
        ++(ok ? m_colorReads : m_colorReadsFailed);
        std::lock_guard lock(m.mutex);
        const auto e = m.entries.find(id);
        if (e == m.entries.end())
            return;
        e->second.finished = true;
        e->second.ok = ok;
        e->second.rgba = std::move(rgba);
    };
    const uint32_t s = std::max<uint32_t>(m_frameScale, 1u);
    for (const auto &[id, read] : waiting)
    {
        // The scene, or an offscreen target, as the last frame left it.
        SDL_GPUTexture *texture = nullptr;
        uint32_t texW = 0, texH = 0;
        if (read.target == m_sceneTarget || read.target == 0u)
        {
            texture = m_scene;
            texW = m_sceneWidth;
            texH = m_sceneHeight;
        }
        else if (const auto t = m_gpuTargets.find(read.target); t != m_gpuTargets.end())
        {
            texture = t->second.texture;
            texW = t->second.width;
            texH = t->second.height;
        }
        const Rect &r = read.rect;
        const uint32_t w = uint32_t(std::max(r.x1 - r.x0, 0)), h = uint32_t(std::max(r.y1 - r.y0, 0));
        if (!texture || m_frameScale == 0u || w == 0u || h == 0u || r.x0 < 0 || r.y0 < 0 ||
            uint32_t(r.x1) * s > texW || uint32_t(r.y1) * s > texH)
        {
            finish(id, {}, false);
            continue;
        }
        // Halved on the GPU while the scale is even (a bilinear blit at
        // half size is the mean of each 2 x 2), so a read at 4x downloads
        // the game's size, not 16 times it; an odd factor left (3x, 6x's 3)
        // is averaged below. The EE waits on this read.
        SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
        std::vector<SDL_GPUTexture *> halves;
        // Kept on the GPU as well, at this resolution (ColorRead::keep).
        if (read.keep != 0u && commands)
        {
            bool fresh = false;
            std::string keepError;
            if (SDL_GPUTexture *keep = targetTexture(device, read.keep, false, w * s, h * s, fresh, keepError))
            {
                SDL_GPUBlitInfo blit{};
                blit.source.texture = texture;
                blit.source.x = uint32_t(r.x0) * s;
                blit.source.y = uint32_t(r.y0) * s;
                blit.source.w = w * s;
                blit.source.h = h * s;
                blit.destination.texture = keep;
                blit.destination.w = w * s;
                blit.destination.h = h * s;
                blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
                blit.filter = SDL_GPU_FILTER_NEAREST;
                SDL_BlitGPUTexture(commands, &blit);
            }
        }
        SDL_GPUTexture *from = texture;
        uint32_t fromX = uint32_t(r.x0) * s, fromY = uint32_t(r.y0) * s;
        uint32_t left = s;
        while (commands && left % 2u == 0u)
        {
            const uint32_t halfW = w * left / 2u, halfH = h * left / 2u;
            SDL_GPUTextureCreateInfo halfInfo{};
            halfInfo.type = SDL_GPU_TEXTURETYPE_2D;
            halfInfo.format = kColorFormat;
            halfInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            halfInfo.width = halfW;
            halfInfo.height = halfH;
            halfInfo.layer_count_or_depth = 1;
            halfInfo.num_levels = 1;
            SDL_GPUTexture *half = SDL_CreateGPUTexture(device, &halfInfo);
            if (!half)
                break;
            halves.push_back(half);
            SDL_GPUBlitInfo blit{};
            blit.source.texture = from;
            blit.source.x = fromX;
            blit.source.y = fromY;
            blit.source.w = w * left;
            blit.source.h = h * left;
            blit.destination.texture = half;
            blit.destination.w = halfW;
            blit.destination.h = halfH;
            blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
            blit.filter = SDL_GPU_FILTER_LINEAR;
            SDL_BlitGPUTexture(commands, &blit);
            from = half;
            fromX = fromY = 0u;
            left /= 2u;
        }
        const uint32_t hostW = w * left, hostH = h * left, bytes = hostW * hostH * 8u;
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        info.size = bytes;
        SDL_GPUTransferBuffer *download = commands ? SDL_CreateGPUTransferBuffer(device, &info) : nullptr;
        SDL_GPUCopyPass *pass = download ? SDL_BeginGPUCopyPass(commands) : nullptr;
        SDL_GPUFence *fence = nullptr;
        if (pass)
        {
            SDL_GPUTextureRegion region{};
            region.texture = from;
            region.x = fromX;
            region.y = fromY;
            region.w = hostW;
            region.h = hostH;
            region.d = 1u;
            SDL_GPUTextureTransferInfo destination{};
            destination.transfer_buffer = download;
            destination.pixels_per_row = hostW;
            destination.rows_per_layer = hostH;
            SDL_DownloadFromGPUTexture(pass, &region, &destination);
            SDL_EndGPUCopyPass(pass);
            fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        }
        else if (commands)
            SDL_CancelGPUCommandBuffer(commands);
        std::vector<float> rgba;
        bool ok = false;
        if (fence)
        {
            SDL_WaitForGPUFences(device, true, &fence, 1u);
            SDL_ReleaseGPUFence(device, fence);
            if (const auto *p = static_cast<const uint16_t *>(SDL_MapGPUTransferBuffer(device, download, false)))
            {
                // Each game pixel the mean of the left x left it covers, in
                // the target's units.
                rgba.resize(size_t(w) * h * 4u);
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                        for (uint32_t c = 0; c < 4u; ++c)
                        {
                            float sum = 0.0f;
                            for (uint32_t dy = 0; dy < left; ++dy)
                                for (uint32_t dx = 0; dx < left; ++dx)
                                    sum += halfToFloat(p[(size_t(y * left + dy) * hostW + x * left + dx) * 4u + c]);
                            rgba[(size_t(y) * w + x) * 4u + c] = sum / float(left * left);
                        }
                SDL_UnmapGPUTransferBuffer(device, download);
                ok = true;
            }
        }
        for (SDL_GPUTexture *half : halves)
            SDL_ReleaseGPUTexture(device, half);
        if (download)
            SDL_ReleaseGPUTransferBuffer(device, download);
        if (!ok)
            std::fprintf(stderr, "[native-pipeline] colour read %u failed: %s\n", id, SDL_GetError());
        finish(id, std::move(rgba), ok);
    }
    m.done.notify_all();
}

void Renderer::releaseDepth(SDL_GPUDevice *device)
{
    if (m_depthFence)
    {
        SDL_WaitForGPUFences(device, true, &m_depthFence, 1u);
        SDL_ReleaseGPUFence(device, m_depthFence);
    }
    if (m_depthDownload)
        SDL_ReleaseGPUTransferBuffer(device, m_depthDownload);
    m_depthFence = nullptr;
    m_depthDownload = nullptr;
    m_depthDownloadBytes = 0u;
    m_depthCopied.clear();
}

void Renderer::release(SDL_GPUDevice *device)
{
    if (!device)
        return;
    releaseMeshes(device);
    releaseDepth(device);
    m_meshes.clear();
    m_frameMeshes.clear();
    m_clears.clear();
    m_frameClears.clear();
    if (m_scene)
        SDL_ReleaseGPUTexture(device, m_scene);
    if (m_output)
        SDL_ReleaseGPUTexture(device, m_output);
    m_output = nullptr;
    if (m_resolvePipeline)
        SDL_ReleaseGPUGraphicsPipeline(device, m_resolvePipeline);
    m_resolvePipeline = nullptr;
    if (m_resolveSampler)
        SDL_ReleaseGPUSampler(device, m_resolveSampler);
    m_resolveSampler = nullptr;
    for (auto &[id, image] : m_images)
        SDL_ReleaseGPUTexture(device, image.texture);
    for (auto &[id, image] : m_palettes)
        SDL_ReleaseGPUTexture(device, image.texture);
    if (m_viewPipeline)
        SDL_ReleaseGPUGraphicsPipeline(device, m_viewPipeline);
    if (m_viewSampler)
        SDL_ReleaseGPUSampler(device, m_viewSampler);
    m_images.clear();
    m_palettes.clear();
    m_imageOrder.clear();
    m_recent.clear();
    m_imageBytes = 0u;
    m_scene = nullptr;
    m_viewPipeline = nullptr;
    m_viewSampler = nullptr;
    m_sceneWidth = m_sceneHeight = 0;
}
} // namespace dq8::gfx::native
