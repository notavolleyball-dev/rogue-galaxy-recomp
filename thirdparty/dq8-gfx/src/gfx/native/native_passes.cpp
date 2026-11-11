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

// The game's blend presets (the table at 0x449EF0; GsPktSetBlendMode):
// (A - B) C + D as the renderer's factors.
Material presetBlend(Material m, int preset)
{
    m.blending = true;
    m.blendOp = BlendOp::Add;
    switch (preset)
    {
    case 1: m.srcFactor = BlendFactor::SrcAlpha, m.dstFactor = BlendFactor::InvSrcAlpha; break; // (Cs - Cd) As + Cd
    case 2: m.srcFactor = BlendFactor::SrcAlpha, m.dstFactor = BlendFactor::One; break;         // Cs As + Cd
    case 3:                                                                                      // Cd - Cs As
        m.srcFactor = BlendFactor::SrcAlpha, m.dstFactor = BlendFactor::One;
        m.blendOp = BlendOp::ReverseSubtract;
        break;
    case 5: m.srcFactor = BlendFactor::Zero, m.dstFactor = BlendFactor::One; break;         // Cd
    case 6: m.srcFactor = BlendFactor::Zero, m.dstFactor = BlendFactor::SrcAlpha; break;    // Cd As
    case 7: m.srcFactor = BlendFactor::Zero, m.dstFactor = BlendFactor::InvSrcAlpha; break; // Cd (1 - As)
    case 8: m.srcFactor = BlendFactor::SrcAlpha, m.dstFactor = BlendFactor::Zero; break;    // Cs As
    case 9: m.srcFactor = BlendFactor::One, m.dstFactor = BlendFactor::One; break;          // Cs + Cd
    case 10:                                                                                 // Cd - Cs
        m.srcFactor = BlendFactor::One, m.dstFactor = BlendFactor::One;
        m.blendOp = BlendOp::ReverseSubtract;
        break;
    default: m.srcFactor = BlendFactor::One, m.dstFactor = BlendFactor::Zero; break; // 0, 4, 11: Cs
    }
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
    case PassKind::ColorFilter:
    {
        // ScreenColorFilterMaybe_179c60: the frame's alpha rebuilt from its
        // green (the GS's CT16 view of the frame drawn onto itself: alpha's
        // bits 2-7 from green's), then its color set to params 0-2 times
        // that alpha (Cs Ad), its alpha 1. params 3: the shuffle's scale.
        const Id frame = pass.frame;
        float fw = 0.0f, fh = 0.0f;
        if (!size(frame, fw, fh))
            return false;
        const Id scratch = passScratch(1u, frame);
        if (scratch == 0u)
            return false;
        const float whole[4] = {0.0f, 0.0f, fw, fh};
        Material copy = spriteMaterial();
        copy.linear = false;
        passRect(scratch, frame, whole, whole, 1.0f, copy);
        Material shuffle = copy;
        shuffle.function = TextureFunction::Decal;
        shuffle.shuffle = ChannelShuffle::GreenToAlpha;
        shuffle.shuffleScale = pass.params[3];
        shuffle.colorWrite = 0x8u;
        passRect(frame, scratch, whole, whole, 1.0f, shuffle);
        Material tint = spriteMaterial();
        tint.blending = true;
        tint.srcFactor = BlendFactor::DstAlpha;
        tint.dstFactor = BlendFactor::Zero;
        const float none[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const float rgba[4] = {pass.params[0], pass.params[1], pass.params[2], 1.0f};
        passRect(frame, 0u, whole, none, rgba, tint);
        return true;
    }
    case PassKind::Capture:
    {
        // The frame copied whole into work[0], scaled to its size,
        // unblended (RefractionCaptureFrame_13ffd0: the picture the
        // refraction materials sample).
        const Id frame = pass.frame, into = pass.work[0];
        float fw = 0.0f, fh = 0.0f, w = 0.0f, h = 0.0f;
        if (!size(frame, fw, fh) || !size(into, w, h))
            return false;
        const float rect[4] = {0.0f, 0.0f, w, h};
        const float uv[4] = {0.0f, 0.0f, fw, fh};
        passRect(into, frame, rect, uv, 1.0f, spriteMaterial());
        return true;
    }
    case PassKind::ShadowMap:
    {
        // ShadowMapBlur_145b60: the map darkened into its partner and back,
        // eight additive blurs between the two (each reads the other, read
        // without its alpha: 1), then the map's red and green copied onto
        // its blue and alpha (the CT16 view's copy, unmasked), and its
        // one-pixel border cleared.
        const Id map = pass.work[0], partner = pass.work[1];
        float w = 0.0f, h = 0.0f, pw = 0.0f, ph = 0.0f;
        if (!size(map, w, h) || !size(partner, pw, ph))
            return false;
        const Id scratch = passScratch(2u, map);
        if (scratch == 0u)
            return false;
        const float whole[4] = {0.0f, 0.0f, w, h};
        Material copy = spriteMaterial();
        passRect(partner, map, whole, whole, pass.params[0], copy);
        passRect(map, partner, whole, whole, 1.0f, copy);
        Material blur = additive(spriteMaterial());
        blur.sourceAlpha = 1.0f;
        for (int i = 0; i < 8; ++i)
        {
            const Id into = (i % 2) == 0 ? partner : map, from = (i % 2) == 0 ? map : partner;
            const int j = i % 4;
            const float ox = pass.params[2 + 2 * j], oy = pass.params[3 + 2 * j];
            const float rect[4] = {ox, oy, w + ox, h + oy};
            passRect(into, from, rect, whole, pass.params[1], blur);
        }
        Material plain = spriteMaterial();
        plain.linear = false;
        passRect(scratch, map, whole, whole, 1.0f, plain);
        Material swap = plain;
        swap.function = TextureFunction::Decal;
        swap.shuffle = ChannelShuffle::RedGreenToBlueAlpha;
        passRect(map, scratch, whole, whole, 1.0f, swap);
        const float none[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const float borders[4][4] = {{0.0f, 0.0f, w, 1.0f},
                                     {w - 1.0f, 0.0f, w, h},
                                     {0.0f, h - 1.0f, w, h},
                                     {0.0f, 0.0f, 1.0f, h}};
        for (const auto &r : borders)
        {
            passRect(map, 0u, r, none, 0.0f, spriteMaterial());
            for (Vertex &v : m_meshes.back().vertices)
                v.color[3] = 0.0f;
        }
        return true;
    }
    case PassKind::DepthOfField:
    {
        // DofFilterDrawMaybe_17d150. Each level: the one before (first the
        // frame) copied into the next rectangle of the work buffer, half the
        // frame's size then 2/3 of the one before, side by side down and
        // right, half a pixel up and left, bilinear; then laid over the
        // frame as a strip of 16 columns, alpha-blended at params 1, only
        // where the scene is behind it (depth test GEQUAL, no depth write),
        // its depth alternating column by column between the level's two.
        // The arithmetic is the game's, in sixteenths of a pixel.
        const Id frame = pass.frame, work = pass.work[0], depth = pass.depth;
        float fw = 0.0f, fh = 0.0f, ww = 0.0f, wh = 0.0f;
        if (!size(frame, fw, fh) || !size(work, ww, wh) || depth == 0u)
            return false;
        const Id scratch = passScratch(3u, work);
        if (scratch == 0u)
            return false;
        struct Rect16
        {
            int32_t u0, v0, u1, v1;
        };
        const int32_t w16 = int32_t(fw) * 16, h16 = int32_t(fh) * 16;
        Rect16 c{0, 0, int32_t(fw) * 8, int32_t(fh) * 8};
        Rect16 d{0, 0, w16, h16};
        Id source = frame;
        const int levels = std::clamp(int(pass.params[0]), 0, 7);
        for (int i = 0; i < levels; ++i)
        {
            // The copy: from the source's rectangle d into c, 8 up and left.
            Material copy = spriteMaterial();
            const float uv[4] = {d.u0 / 16.0f, d.v0 / 16.0f, d.u1 / 16.0f, d.v1 / 16.0f};
            if (source == work)
            {
                // The level before is the work buffer's own: through the
                // scratch, so no draw reads the target it writes.
                Material plain = copy;
                plain.linear = false;
                passRect(scratch, work, uv, uv, 1.0f, plain);
                source = scratch;
            }
            const float into[4] = {(c.u0 - 8) / 16.0f, (c.v0 - 8) / 16.0f, (c.u1 - 8) / 16.0f, (c.v1 - 8) / 16.0f};
            passRect(work, source, into, uv, 1.0f, copy);
            // The strip over the frame.
            MeshDraw strip;
            reuseMeshBuffers(strip);
            strip.target = frame;
            strip.depthTarget = depth;
            strip.hasModel = false;
            strip.scissor = {0, 0, int32_t(fw), int32_t(fh)};
            Material &m = strip.material;
            m = spriteMaterial();
            m.sourceTarget = work;
            m.sourceAlpha = 1.0f; // read with TEXA's 0x80
            m.blending = true;
            m.srcFactor = BlendFactor::SrcAlpha;
            m.dstFactor = BlendFactor::InvSrcAlpha;
            m.depthTest = Compare::GreaterEqual;
            m.depthWrite = false;
            m.colorWrite = 0x7u;
            const float depths[2] = {pass.params[2 + 2 * i], pass.params[3 + 2 * i]};
            const float dx = float(w16) / 16.0f, du = float(c.u1 - c.u0) / 16.0f;
            float xx = 0.0f, uu = float(c.u0);
            int sel = i % 2;
            const auto column = [&](float x16, float u16, float z) {
                for (int edge = 0; edge < 2; ++edge)
                {
                    Vertex v;
                    v.position[0] = float(int32_t(x16)) / 16.0f;
                    v.position[1] = edge == 0 ? 0.0f : fh;
                    v.position[2] = z;
                    v.uv[0] = float(int32_t(u16)) / 16.0f;
                    v.uv[1] = edge == 0 ? (c.v0 + 16) / 16.0f : (c.v1 - 16) / 16.0f;
                    v.color[0] = v.color[1] = v.color[2] = 1.0f;
                    v.color[3] = pass.params[1];
                    strip.vertices.push_back(v);
                }
            };
            column(xx, uu, depths[0]);
            while (xx < float(w16))
            {
                sel = !sel;
                xx += dx;
                uu += du;
                column(xx, uu, depths[sel]);
            }
            for (uint32_t k = 0; k + 3 < strip.vertices.size(); k += 2)
                strip.indices.insert(strip.indices.end(), {k, k + 1, k + 2, k + 1, k + 3, k + 2});
            strip.source = 0x50415353u;
            m_meshes.push_back(std::move(strip));
            // The next level.
            d = c;
            const int32_t du16 = d.u1 - d.u0, dv16 = d.v1 - d.v0;
            c.u0 += du16;
            c.u1 = c.u0 + du16 * 2 / 3;
            c.v0 += dv16;
            c.v1 = c.v0 + dv16 * 2 / 3;
            source = work;
        }
        return true;
    }
    case PassKind::Focus:
    {
        // FocusFilterDrawMaybe_17c000: the frame's alpha as a mask by depth
        // band, the frame blurred twice (into A by lerps, into C by adds,
        // C's red and green its blue and alpha: the mask blurred), A's alpha
        // C's, A laid over the frame by its alpha.
        const Id frame = pass.frame, a = pass.work[0], a2 = pass.work[1], c = pass.work[2], c2 = pass.work[3];
        const Id depth = pass.depth;
        float fw = 0.0f, fh = 0.0f, aw = 0.0f, ah = 0.0f, cw = 0.0f, ch = 0.0f, t0 = 0.0f, t1 = 0.0f;
        if (!size(frame, fw, fh) || !size(a, aw, ah) || !size(c, cw, ch) || !size(a2, t0, t1) ||
            !size(c2, t0, t1) || depth == 0u)
            return false;
        const Id scratch = passScratch(4u, c);
        if (scratch == 0u)
            return false;
        const float none[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const float frameRect[4] = {0.0f, 0.0f, fw, fh};
        // 1. The frame's alpha: 1, then each band's where the scene is
        // behind its depth.
        {
            Material m = spriteMaterial();
            m.colorWrite = 0x8u;
            passRect(frame, 0u, frameRect, none, 1.0f, m);
            for (int i = 0; i < 4; ++i)
            {
                Material band = m;
                band.depthTest = Compare::GreaterEqual;
                const float rgba[4] = {0.0f, 0.0f, 0.0f, pass.params[4 + i]};
                passRect(frame, 0u, frameRect, none, rgba, band);
                MeshDraw &d = m_meshes.back();
                d.depthTarget = depth;
                for (Vertex &v : d.vertices)
                    v.position[2] = pass.params[i];
            }
        }
        // 2. The frame into A and into C, half a pixel up and left; C's
        // color the mask (Cs Ad), its alpha 1; then their partners copied.
        {
            const float intoA[4] = {-0.5f, -0.5f, aw - 0.5f, ah - 0.5f};
            const float intoC[4] = {-0.5f, -0.5f, cw - 0.5f, ch - 0.5f};
            passRect(a, frame, intoA, frameRect, 1.0f, spriteMaterial());
            passRect(c, frame, intoC, frameRect, 1.0f, spriteMaterial());
            Material mask = spriteMaterial();
            mask.blending = true;
            mask.srcFactor = BlendFactor::DstAlpha;
            mask.dstFactor = BlendFactor::Zero;
            const float wholeC[4] = {0.0f, 0.0f, cw, ch};
            passRect(c, 0u, wholeC, none, pass.params[9], mask);
            Material plain = spriteMaterial();
            plain.linear = false;
            const float wholeA[4] = {0.0f, 0.0f, aw, ah};
            passRect(a2, a, wholeA, wholeA, 1.0f, plain);
            passRect(c2, c, wholeC, wholeC, 1.0f, plain);
        }
        const float dx[4] = {0.0f, -1.0f, 0.0f, 1.0f}, dy[4] = {-1.0f, 0.0f, 1.0f, 0.0f};
        // 3. Four rounds between A and A': each the other shifted, lerped
        // in by params 8 (A=Cs B=Cd C=FIX D=Cd), alpha kept.
        {
            Material lerp = spriteMaterial();
            lerp.blending = true;
            lerp.srcFactor = BlendFactor::Fixed;
            lerp.dstFactor = BlendFactor::InvFixed;
            lerp.blendConstant = pass.params[8];
            lerp.colorWrite = 0x7u;
            const float uv[4] = {0.0f, 0.0f, aw, ah};
            for (int q = 0; q < 4; ++q)
            {
                const Id into = (q % 2) == 0 ? a2 : a, from = (q % 2) == 0 ? a : a2;
                const float o = pass.params[11];
                const float rect[4] = {dx[q] * o - 0.5f, dy[q] * o - 0.5f, aw + dx[q] * o - 0.5f, ah + dy[q] * o - 0.5f};
                passRect(into, from, rect, uv, 1.0f, lerp);
            }
        }
        // 4. Four rounds between C and C', added, alpha kept.
        {
            Material add = additive(spriteMaterial());
            add.colorWrite = 0x7u;
            const float uv[4] = {0.0f, 0.0f, cw, ch};
            for (int q = 0; q < 4; ++q)
            {
                const Id into = (q % 2) == 0 ? c2 : c, from = (q % 2) == 0 ? c : c2;
                const float o = pass.params[12];
                const float rect[4] = {dx[q] * o - 0.5f, dy[q] * o - 0.5f, cw + dx[q] * o - 0.5f, ch + dy[q] * o - 0.5f};
                passRect(into, from, rect, uv, 1.0f, add);
            }
        }
        // 5. C's red and green onto its blue and alpha (its CT16 view's
        // copy); A's alpha C's (scaled to A); A over the frame by its alpha
        // at params 10, the frame's alpha kept.
        {
            const float wholeC[4] = {0.0f, 0.0f, cw, ch};
            Material plain = spriteMaterial();
            plain.linear = false;
            passRect(scratch, c, wholeC, wholeC, 1.0f, plain);
            Material swap = plain;
            swap.function = TextureFunction::Decal;
            swap.shuffle = ChannelShuffle::RedGreenToBlueAlpha;
            passRect(c, scratch, wholeC, wholeC, 1.0f, swap);
            Material alphaOnly = spriteMaterial();
            alphaOnly.colorWrite = 0x8u;
            const float intoA[4] = {-0.5f, -0.5f, aw - 0.5f, ah - 0.5f};
            passRect(a, c, intoA, wholeC, 1.0f, alphaOnly);
            Material over = spriteMaterial();
            over.blending = true;
            over.srcFactor = BlendFactor::SrcAlpha;
            over.dstFactor = BlendFactor::InvSrcAlpha;
            over.colorWrite = 0x7u;
            const float uv[4] = {0.0f, 0.0f, aw, ah};
            const float rgba[4] = {1.0f, 1.0f, 1.0f, pass.params[10]};
            passRect(frame, a, frameRect, uv, rgba, over);
        }
        return true;
    }
    case PassKind::CBlur:
    {
        // CBlurTexBlurMaybe_1789e0. P's alpha is the trail's mask (CBlurDraw
        // wrote it); its color becomes that mask (0xFF times its alpha),
        // then either P's own taps added (feedback) or the history Q, P as
        // it was before, added a pixel over at the strength; then P's red
        // and green onto its blue and alpha, so its alpha is the trail.
        const Id p = pass.work[0], q = pass.work[1];
        float pw = 0.0f, ph = 0.0f, qw = 0.0f, qh = 0.0f;
        if (!size(p, pw, ph) || !size(q, qw, qh))
            return false;
        const Id scratch = passScratch(7u, p);
        if (scratch == 0u)
            return false;
        const bool feedback = pass.params[0] != 0.0f;
        const float strength = pass.params[1], clampTo = pass.params[2];
        const float whole[4] = {0.0f, 0.0f, pw, ph}, none[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        Material plain = spriteMaterial();
        plain.linear = false;
        if (!feedback)
            passRect(q, p, whole, whole, 1.0f, plain); // GfxBlitTexRectPixelsAt: P into Q
        {
            Material fill = spriteMaterial();
            fill.colorWrite = 0x7u;
            passRect(p, 0u, whole, none, clampTo, fill);
            Material byAlpha = fill;
            byAlpha.blending = true;
            byAlpha.srcFactor = BlendFactor::DstAlpha; // A=Cs B=0 C=Ad D=0
            byAlpha.dstFactor = BlendFactor::Zero;
            passRect(p, 0u, whole, none, clampTo, byAlpha);
        }
        Material add = presetBlend(spriteMaterial(), 9);
        add.colorWrite = 0x7u;
        if (feedback)
        {
            // Eight of P's taps onto itself (the GS read what it had drawn:
            // each tap from P as the one before left it).
            for (int i = 0; i < 8; ++i)
            {
                Material copy = plain;
                copy.colorClamp = clampTo;
                passRect(scratch, p, whole, whole, 1.0f, copy);
                const float u = pass.params[3 + 2 * (i % 4)], v = pass.params[4 + 2 * (i % 4)];
                const float uv[4] = {u, v, pw + u, ph + v};
                const float rgba[4] = {0x20 / 128.0f, 0x20 / 128.0f, 0x20 / 128.0f, 1.0f};
                passRect(p, scratch, whole, uv, rgba, add);
            }
        }
        else
        {
            const float uv[4] = {1.0f, 1.0f, pw + 1.0f, ph + 1.0f};
            passRect(p, q, whole, uv, strength, add);
        }
        Material clamped = plain;
        clamped.colorClamp = clampTo;
        passRect(scratch, p, whole, whole, 1.0f, clamped);
        Material swap = plain;
        swap.function = TextureFunction::Decal;
        swap.shuffle = ChannelShuffle::RedGreenToBlueAlpha;
        passRect(p, scratch, whole, whole, 1.0f, swap);
        return true;
    }
    case PassKind::CBlurDraw:
    {
        // CBlurDrawMaybe_1784e0. S is the frame's copy, or a small sprite
        // made white; its alpha becomes the trail P's, shifted; S then laid
        // over the frame by the call's preset, shifted back, the frame's
        // alpha kept.
        const Id frame = pass.frame, p = pass.work[0], sprite = pass.work[1];
        float fw = 0.0f, fh = 0.0f, pw = 0.0f, ph = 0.0f, sw = 0.0f, sh = 0.0f;
        if (!size(frame, fw, fh) || !size(p, pw, ph) || !size(sprite, sw, sh))
            return false;
        const float dx = pass.params[0], dy = pass.params[1];
        const bool small = pass.params[3] != 0.0f;
        const float W = pass.params[8], H = pass.params[9];
        const float infoW = pass.params[10] > 0.0f ? pass.params[10] : fw;
        const float infoH = pass.params[11] > 0.0f ? pass.params[11] : fh;
        const float spriteRect[4] = {0.0f, 0.0f, sw, sh}, none[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        Material plain = spriteMaterial();
        if (!small)
        {
            // GfxBlitTexRectPixels: the frame into S, W by H.
            const float from[4] = {0.0f, 0.0f, infoW, infoH}, to[4] = {0.0f, 0.0f, W, H};
            passRect(sprite, frame, to, from, 1.0f, plain);
        }
        else
        {
            Material white = spriteMaterial();
            passRect(sprite, 0u, spriteRect, none, 1.0f, white);
        }
        Material alphaOnly = spriteMaterial();
        alphaOnly.colorWrite = 0x8u;
        if (dx != 0.0f || dy != 0.0f)
        {
            const float rgba[4] = {1.0f, 1.0f, 1.0f, 0.0f};
            passRect(sprite, 0u, spriteRect, none, rgba, alphaOnly);
        }
        {
            const float rect[4] = {dx, dy, sw + dx, sh + dy}, uv[4] = {0.0f, 0.0f, pw, ph};
            passRect(sprite, p, rect, uv, 1.0f, alphaOnly);
        }
        Material over = presetBlend(spriteMaterial(), int(pass.params[2]));
        over.colorWrite = 0x7u;
        const float rect[4] = {-1.0f - dx, -2.0f - dy, W - dx - 1.0f, H - dy - 2.0f};
        const float rgba[4] = {pass.params[4], pass.params[5], pass.params[6], pass.params[7]};
        passRect(frame, sprite, rect, spriteRect, rgba, over);
        return true;
    }
    case PassKind::ObjectLayer:
    {
        // ModelDrawOffscreenCompositeMaybe_16aac0's tail: the object's layer
        // laid over the frame (and its glow copies), or into its blur's
        // trail, alpha only. The VU1 overlay program's sprites cover the
        // object's vertex bounds within the rectangle; the rest of the
        // rectangle is the layer's clear (alpha 0), which the alpha test
        // drops, so the rectangle is drawn whole.
        const bool intoTrail = pass.params[1] != 0.0f;
        const Id source = pass.work[0], target = intoTrail ? pass.work[1] : pass.frame;
        float sw = 0.0f, sh = 0.0f, tw = 0.0f, th = 0.0f;
        if (source == 0u || target == 0u || !size(source, sw, sh) || !size(target, tw, th))
            return false;
        Material m = spriteMaterial();
        const int blend = int(pass.params[14]);
        if (blend == 1)
            m = presetBlend(m, 1);
        else if (blend == 2)
        {
            m.blending = true;
            m.srcFactor = BlendFactor::Fixed;
            m.dstFactor = BlendFactor::InvFixed;
            m.blendConstant = pass.params[15];
        }
        m.alphaTest = int(pass.params[16]) == 6 ? Compare::Greater : Compare::GreaterEqual;
        m.alphaReference = pass.params[17];
        m.alphaFail = AlphaFail::Nothing;
        if (pass.params[18] != 0.0f)
        {
            m.sourceAlpha = 1.0f; // TEXA TA0 0x80
            m.alphaZeroOnBlack = true;
        }
        if (intoTrail)
            m.colorWrite = 0x8u;
        const float uv[4] = {pass.params[6], pass.params[7], pass.params[8], pass.params[9]};
        const float rgba[4] = {pass.params[10], pass.params[11], pass.params[12], pass.params[13]};
        const int offsets = std::clamp(int(pass.params[19]), 1, 4);
        for (int k = 0; k < offsets; ++k)
        {
            const float dx = pass.params[20 + 2 * k], dy = pass.params[21 + 2 * k];
            const float rect[4] = {pass.params[2] + dx, pass.params[3] + dy, pass.params[4] + dx, pass.params[5] + dy};
            passRect(target, source, rect, uv, rgba, m);
        }
        return true;
    }
    case PassKind::PlaneShadow:
    {
        // DrawListEndPlaneShadowPass_146cc0. The volumes were drawn into the
        // mask (VU1 meshes, before the pass); the mask less 1 (clamped at 0,
        // as the GS's blend was); then either the frame darkened where the
        // mask is not black, or the mask blurred into its copy (the mask's
        // shadowed texels 0x27 strong, six shifted adds), and the copy laid
        // over the frame darkening, colored, or subtracting.
        const Id frame = pass.frame, mask = pass.work[0], blurred = pass.work[1];
        float fw = 0.0f, fh = 0.0f, mw = 0.0f, mh = 0.0f, bw = 0.0f, bh = 0.0f;
        if (!size(frame, fw, fh) || !size(mask, mw, mh) || !size(blurred, bw, bh))
            return false;
        const Id scratchMask = passScratch(5u, mask), scratch = passScratch(6u, blurred);
        if (scratchMask == 0u || scratch == 0u)
            return false;
        const float strength = pass.params[0], clampTo = pass.params[5];
        const int flags = int(pass.params[4]);
        const float none[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const float maskRect[4] = {0.0f, 0.0f, mw, mh}, frameRect[4] = {0.0f, 0.0f, fw, fh};
        const float blurRect[4] = {0.0f, 0.0f, bw, bh};
        // The mask less 1, clamped through the scratch.
        {
            Material less = spriteMaterial();
            less.blending = true;
            less.srcFactor = BlendFactor::One;
            less.dstFactor = BlendFactor::One;
            less.blendOp = BlendOp::ReverseSubtract;
            passRect(mask, 0u, maskRect, none, 1.0f, less);
            Material copy = spriteMaterial();
            copy.linear = false;
            copy.colorClamp = clampTo;
            passRect(scratchMask, mask, maskRect, maskRect, 1.0f, copy);
            passRect(mask, scratchMask, maskRect, maskRect, 1.0f, copy);
        }
        if ((flags & 1) == 0)
        {
            // The frame times (1 - the strength) where the mask is not black
            // (read without its alpha: the strength, 0 on black; alpha test
            // GEQUAL 1/128).
            Material dark = spriteMaterial();
            dark.sourceAlpha = strength;
            dark.alphaZeroOnBlack = true;
            dark.alphaTest = Compare::GreaterEqual;
            dark.alphaReference = 1.0f / 128.0f;
            dark.alphaFail = AlphaFail::Nothing;
            dark.blending = true;
            dark.srcFactor = BlendFactor::Zero;
            dark.dstFactor = BlendFactor::InvSrcAlpha;
            passRect(frame, mask, frameRect, maskRect, 1.0f, dark);
            return true;
        }
        // The mask into its copy: its shadowed texels at 0x27, as
        // HIGHLIGHT2 (Ct + Af) times that alpha, three quarters of a pixel
        // up and left.
        {
            Material in = spriteMaterial();
            in.sourceAlpha = 0x27 / 128.0f;
            in.alphaZeroOnBlack = true;
            in.function = TextureFunction::Highlight2;
            in.blending = true;
            in.srcFactor = BlendFactor::SrcAlpha;
            in.dstFactor = BlendFactor::Zero;
            const float rect[4] = {-0.75f, -0.75f, bw - 0.75f, bh - 0.75f};
            passRect(blurred, mask, rect, maskRect, 1.0f, in);
        }
        // Six adds of itself, shifted (through the scratch).
        {
            Material plain = spriteMaterial();
            plain.linear = false;
            Material add = additive(spriteMaterial());
            const float rgba[4] = {0x24 / 128.0f, 0x24 / 128.0f, 0x24 / 128.0f, 0x10 / 128.0f};
            for (int k = 0; k < 6; ++k)
            {
                passRect(scratch, blurred, blurRect, blurRect, 1.0f, plain);
                const float ox = pass.params[6 + 2 * k], oy = pass.params[7 + 2 * k];
                const float rect[4] = {ox, oy, bw + ox, bh + oy};
                passRect(blurred, scratch, rect, blurRect, rgba, add);
            }
            Material clamped = plain;
            clamped.colorClamp = clampTo;
            passRect(scratch, blurred, blurRect, blurRect, 1.0f, clamped);
            if (flags & 2)
            {
                Material swap = plain;
                swap.function = TextureFunction::Decal;
                swap.shuffle = ChannelShuffle::RedGreenToBlueAlpha;
                passRect(blurred, scratch, blurRect, blurRect, 1.0f, swap);
            }
            else
                passRect(blurred, scratch, blurRect, blurRect, 1.0f, plain);
        }
        // The copy over the frame, the frame's alpha kept.
        {
            Material over = spriteMaterial();
            over.blending = true;
            over.colorWrite = 0x7u;
            float rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            if ((flags & 2) && (flags & 4))
            {
                for (int c = 0; c < 3; ++c)
                    rgba[c] = float(int32_t(128.0f * pass.params[1 + c])) / 128.0f;
                rgba[3] = float(int32_t(128.0f * (1.0f - strength))) / 128.0f;
                over.srcFactor = BlendFactor::SrcAlpha; // preset 1: (Cs - Cd) As + Cd
                over.dstFactor = BlendFactor::InvSrcAlpha;
            }
            else if (flags & 2)
            {
                rgba[3] = float(int32_t(128.0f * strength)) / 128.0f;
                over.srcFactor = BlendFactor::Zero; // preset 7: Cd (1 - As)
                over.dstFactor = BlendFactor::InvSrcAlpha;
            }
            else
            {
                const float c = float(int32_t(128.0f * strength)) / 128.0f;
                rgba[0] = rgba[1] = rgba[2] = c;
                over.srcFactor = BlendFactor::One; // preset 10: Cd - Cs
                over.dstFactor = BlendFactor::One;
                over.blendOp = BlendOp::ReverseSubtract;
            }
            passRect(frame, blurred, frameRect, blurRect, rgba, over);
        }
        return true;
    }
    default:
        return false;
    }
}

} // namespace dq8::gfx::native
