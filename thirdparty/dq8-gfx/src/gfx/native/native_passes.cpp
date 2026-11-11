// The native pipeline's passes (Rogue Galaxy: added; M3 of the game's
// docs/NATIVE-ONLY.md): each post effect as the renderer's own draws and
// clears, from the hooked function's parameters (native_records.h's Pass).
// What each kind draws follows the game's function, step for step (the
// game's docs/NATIVE-ONLY-NOTES.md has the function), in the renderer's
// units. Where the game drew a buffer onto itself, a scratch target of the
// renderer's own takes the copy, so no draw reads the target it writes.

#include "gfx/native/native_pipeline.h"

#include <algorithm>
#include <cmath>

namespace dq8::gfx::native
{
namespace
{
// The renderer's own targets, from the top of the ids (the producer counts
// up from 1).
constexpr Id kScratchBase = 0xFFFFFF00u;

Material spriteMaterial()
{
    Material m;
    m.function = TextureFunction::Modulate;
    m.textureAlpha = true;
    m.textureWidth = m.textureHeight = 1u; // texel UVs
    m.linear = true;
    m.clampU = m.clampV = true;
    m.colorWrite = 0xFu;
    m.known = true;
    return m;
}

Material additive(Material m)
{
    m.blending = true;
    m.srcFactor = BlendFactor::One;
    m.dstFactor = BlendFactor::One;
    m.blendOp = BlendOp::Add;
    return m;
}
} // namespace

Id Renderer::passScratch(uint32_t slot, Id like)
{
    const Id id = kScratchBase + slot;
    const auto from = m_targets.find(like);
    if (from == m_targets.end())
        return 0u;
    Target &t = m_targets[id];
    if (t.id != id || t.width != from->second.width || t.height != from->second.height)
    {
        t = from->second;
        t.id = id;
        t.role = TargetRole::Offscreen;
    }
    return id;
}

void Renderer::passRect(Id target, Id source, const float rect[4], const float uv[4], float color,
                        const Material &material)
{
    // The game's sprites draw at alpha 0x80: 1.
    const float rgba[4] = {color, color, color, 1.0f};
    passRect(target, source, rect, uv, rgba, material);
}

void Renderer::passRect(Id target, Id source, const float rect[4], const float uv[4], const float rgba[4],
                        const Material &material)
{
    MeshDraw draw;
    reuseMeshBuffers(draw);
    draw.target = target;
    draw.hasModel = false;
    draw.material = material;
    draw.material.sourceTarget = source;
    if (source == 0u)
        draw.material.textureWidth = draw.material.textureHeight = 0u;
    const auto declared = m_targets.find(target);
    const int32_t w = declared != m_targets.end() ? declared->second.width : 0;
    const int32_t h = declared != m_targets.end() ? declared->second.height : 0;
    draw.scissor = {0, 0, w, h};
    const float corners[4][4] = {{rect[0], rect[1], uv[0], uv[1]},
                                 {rect[2], rect[1], uv[2], uv[1]},
                                 {rect[0], rect[3], uv[0], uv[3]},
                                 {rect[2], rect[3], uv[2], uv[3]}};
    draw.vertices.resize(4);
    for (int k = 0; k < 4; ++k)
    {
        Vertex &v = draw.vertices[k];
        v.position[0] = corners[k][0];
        v.position[1] = corners[k][1];
        v.position[2] = 0.0f;
        v.uv[0] = corners[k][2];
        v.uv[1] = corners[k][3];
        for (int c = 0; c < 4; ++c)
            v.color[c] = rgba[c];
    }
    draw.indices = {0u, 1u, 2u, 1u, 3u, 2u};
    draw.source = 0x50415353u; // 'PASS'
    m_meshes.push_back(std::move(draw));
}

bool Renderer::expandPass(const Pass &pass)
{
    const auto size = [&](Id id, float &w, float &h) {
        const auto t = m_targets.find(id);
        if (t == m_targets.end() || t->second.width == 0u || t->second.height == 0u)
            return false;
        w = float(t->second.width);
        h = float(t->second.height);
        return true;
    };
    switch (pass.kind)
    {
    case PassKind::Glare:
    {
        // GlareFilterDrawMaybe_17a0d0. a: the frame's bright parts, b:
        // their streaks; positions in the targets' pixels. The GS clamps
        // every blend's result to [0, its 0xFF]; float targets do not, so b
        // is clamped through the scratch wherever the game's would have
        // been (params[4], the clamp).
        const Id frame = pass.frame, a = pass.work[0], b = pass.work[1];
        float fw = 0.0f, fh = 0.0f, aw = 0.0f, ah = 0.0f, bw = 0.0f, bh = 0.0f;
        if (!size(frame, fw, fh) || !size(a, aw, ah) || !size(b, bw, bh))
            return false;
        const Id scratch = passScratch(0u, b);
        if (scratch == 0u)
            return false;
        const float alphaOver = pass.params[0], streak = pass.params[1], spread = pass.params[2],
                    strength = pass.params[3], clampTo = pass.params[4];
        const float whole[4] = {0.0f, 0.0f, bw, bh};
        // b clamped: copied to the scratch clamped, and back.
        const auto clampB = [&] {
            Material copy = spriteMaterial();
            copy.linear = false;
            copy.colorClamp = clampTo;
            passRect(scratch, b, whole, whole, 1.0f, copy);
            passRect(b, scratch, whole, whole, 1.0f, copy);
        };
        // 1. a cleared, black and opaque.
        Clear clear;
        clear.target = a;
        clear.color = true;
        clear.rgba[3] = 1.0f;
        m_clears.emplace_back(m_meshes.size(), clear);
        // 2. The frame onto a, where its alpha is over the threshold, half a
        // pixel up and left.
        {
            Material m = spriteMaterial();
            m.alphaTest = Compare::Greater;
            m.alphaReference = alphaOver;
            m.alphaFail = AlphaFail::Nothing;
            const float rect[4] = {-0.5f, -0.5f, aw - 0.5f, ah - 0.5f};
            const float uv[4] = {0.0f, 0.0f, fw, fh};
            passRect(a, frame, rect, uv, 1.0f, m);
        }
        // 3. b: half of a times its alpha (the GS's blend preset 8, Cs As),
        // less half of a (0 - Cs, plus Cd): what is left is where the alpha
        // was over 1. A quarter pixel up and left.
        {
            const float rect[4] = {-0.25f, -0.25f, aw - 0.25f, ah - 0.25f};
            const float uv[4] = {0.0f, 0.0f, aw, ah};
            Material scaled = spriteMaterial();
            scaled.blending = true;
            scaled.srcFactor = BlendFactor::SrcAlpha;
            scaled.dstFactor = BlendFactor::Zero;
            passRect(b, a, rect, uv, 0.5f, scaled);
            Material less = spriteMaterial();
            less.blending = true;
            less.srcFactor = BlendFactor::One;
            less.dstFactor = BlendFactor::One;
            less.blendOp = BlendOp::ReverseSubtract;
            passRect(b, a, rect, uv, 0.5f, less);
            clampB();
            // Its border, a pixel wide, black and clear.
            const float none[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            const float borders[4][4] = {{-1.0f, -1.0f, bw + 1.0f, 1.0f},
                                         {-1.0f, -1.0f, 1.0f, bh + 1.0f},
                                         {bw - 1.0f, -1.0f, bw + 1.0f, bh + 1.0f},
                                         {-1.0f, bh - 1.0f, bw + 1.0f, bh + 1.0f}};
            for (const auto &r : borders)
            {
                passRect(b, 0u, r, none, 0.0f, spriteMaterial());
                for (Vertex &v : m_meshes.back().vertices)
                    v.color[3] = 0.0f;
            }
        }
        // 4. Five rounds of streaks: b (clamped, through the scratch) added
        // onto b four times, shifted along both axes and both diagonals by
        // (i + 1) times the spread, each round fainter. The arithmetic is
        // the game's, in its sixteenths of a pixel (integer halves and
        // quarters included).
        {
            const int32_t x0 = -4, y0 = -4, x1 = int32_t(aw) * 16 - 4, y1 = int32_t(ah) * 16 - 4;
            const float uv[4] = {1.0f, 1.0f, aw - 1.0f, ah - 1.0f};
            for (int i = 0; i < 5; ++i)
            {
                clampB();
                const int32_t o = int32_t(float(i + 1) * spread); // spread: sixteenths of a pixel
                const int32_t hi = o - 8, lo = -8 - o;
                const float c = streak * float(5 - i) / 5.0f;
                const int32_t shifts[4][4] = {{x0 + hi / 2, y0 + hi / 2, x1 + hi / 2, y1 + hi / 4},
                                              {x0 + lo / 2, y0 + lo / 2, x1 + lo / 2, y1 + lo / 4},
                                              {x0 + lo, y0 + hi, x1 + lo, y1 + hi},
                                              {x0 + hi, y0 + lo, x1 + hi, y1 + lo}};
                for (const auto &r16 : shifts)
                {
                    const float r[4] = {r16[0] / 16.0f, r16[1] / 16.0f, r16[2] / 16.0f, r16[3] / 16.0f};
                    passRect(b, scratch, r, uv, c, additive(spriteMaterial()));
                }
            }
        }
        // 5. b (clamped) added onto the frame.
        {
            clampB();
            const float rect[4] = {0.0f, 0.0f, fw, fh};
            const float uv[4] = {1.0f, 1.0f, bw - 1.0f, bh - 1.0f};
            passRect(frame, b, rect, uv, strength, additive(spriteMaterial()));
        }
        return true;
    }
    case PassKind::Bloom:
    {
        // BloomExtractBlurMaybe_179360. a: the frame's pixels of alpha under
        // 1 as grey (their alpha), blurred into b and back into a by four
        // diagonal half-strength copies each way, then added onto the
        // frame's color at params 1-3 (its alpha kept). params 0: the blur's
        // offset in sixteenths of a pixel.
        const Id frame = pass.frame, a = pass.work[0], b = pass.work[1];
        float fw = 0.0f, fh = 0.0f, aw = 0.0f, ah = 0.0f, bw = 0.0f, bh = 0.0f;
        if (!size(frame, fw, fh) || !size(a, aw, ah) || !size(b, bw, bh))
            return false;
        const auto clearTo = [&](Id id) {
            Clear clear;
            clear.target = id;
            clear.color = true;
            m_clears.emplace_back(m_meshes.size(), clear);
        };
        // 1. a: where the frame's alpha is under 1, its alpha (half a pixel
        // up and left, nearest); then a's alpha as its color (Cs Ad, 1).
        clearTo(a);
        {
            Material m = spriteMaterial();
            m.linear = false;
            m.alphaTest = Compare::Less;
            m.alphaReference = 1.0f;
            m.alphaFail = AlphaFail::Nothing;
            const float rect[4] = {-0.5f, -0.5f, aw - 0.5f, ah - 0.5f};
            const float uv[4] = {0.0f, 0.0f, fw, fh};
            const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            passRect(a, frame, rect, uv, black, m);
            Material grey = spriteMaterial();
            grey.blending = true;
            grey.srcFactor = BlendFactor::DstAlpha;
            grey.dstFactor = BlendFactor::Zero;
            const float whole[4] = {0.0f, 0.0f, aw, ah};
            const float none[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            passRect(a, 0u, whole, none, 1.0f, grey);
        }
        // 2. a into b, then b into a: each cleared, then four copies of the
        // other at half strength (Cs + Cd), three quarters of a pixel up and
        // left and the offset along each diagonal.
        {
            const float v = pass.params[0] / 16.0f;
            const float ox[4] = {v, v, -v, -v}, oy[4] = {v, -v, v, -v};
            const Id order[2][2] = {{a, b}, {b, a}};
            for (const auto &step : order)
            {
                const Id from = step[0], into = step[1];
                float w = 0.0f, h = 0.0f;
                size(into, w, h);
                clearTo(into);
                const float uv[4] = {0.0f, 0.0f, w, h};
                for (int k = 0; k < 4; ++k)
                {
                    const float rect[4] = {-0.75f + ox[k], -0.75f + oy[k], w - 0.75f + ox[k], h - 0.75f + oy[k]};
                    passRect(into, from, rect, uv, 0.5f, additive(spriteMaterial()));
                }
            }
        }
        // 3. a onto the frame, added, its alpha not written.
        {
            Material m = additive(spriteMaterial());
            m.colorWrite = 0x7u;
            const float rect[4] = {0.0f, 0.0f, fw, fh};
            const float uv[4] = {0.0f, 0.0f, aw, ah};
            const float rgba[4] = {pass.params[1], pass.params[2], pass.params[3], pass.params[4]};
            passRect(frame, a, rect, uv, rgba, m);
        }
        return true;
    }
    default:
        return false;
    }
}

} // namespace dq8::gfx::native
