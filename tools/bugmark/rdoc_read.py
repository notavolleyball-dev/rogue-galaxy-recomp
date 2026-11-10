# Runs inside qrenderdoc's Python (tools/bugmark/bugmark.py starts it): reads
# a bug mark's RenderDoc capture and writes MARK/report/. Not run by hand.
#   BUGMARK_DIR   the mark's folder
#   BUGMARK_AT    "x y" in window pixels, or "uv U V" (0..1 across the
#                 picture); empty takes the mouse from info.txt, if any
import glob
import os
import traceback

mark = os.environ["BUGMARK_DIR"]
report_dir = os.path.join(mark, "report")
os.makedirs(report_dir, exist_ok=True)
out = open(os.path.join(report_dir, "report.txt"), "w")


def say(text=""):
    out.write(text + "\n")


def short(rid):
    return str(rid).replace("ResourceId::", "")


def enum(kind, value):
    try:
        return kind(value).name
    except Exception:
        return str(value).split(".")[-1]


def rgba(value):
    return "(%.3f %.3f %.3f %.3f)" % tuple(value.col.floatValue[0:4])


def main():
    import renderdoc as rd

    info = {}
    for line in open(os.path.join(mark, "info.txt")):
        key, _, value = line.rstrip("\n").partition(" ")
        info.setdefault(key, value)
    captures = sorted(glob.glob(os.path.join(mark, "*.rdc")))
    if not captures:
        say("no RenderDoc capture in %s (was the game started under RenderDoc?)" % mark)
        return
    cap = rd.OpenCaptureFile()
    result = cap.OpenFile(captures[-1], "", None)
    if result != rd.ResultCode.Succeeded:
        say("cannot open %s: %s" % (captures[-1], result))
        return
    result, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        say("cannot replay %s: %s" % (captures[-1], result))
        return
    sd = ctrl.GetStructuredFile()
    textures = {t.resourceId: t for t in ctrl.GetTextures()}
    null = rd.ResourceId.Null()

    actions = []

    def walk(action):
        actions.append(action)
        for child in action.children:
            walk(child)

    for root in ctrl.GetRootActions():
        walk(root)

    def is_draw(a):
        return bool(a.flags & rd.ActionFlags.Drawcall)

    def name(a):
        return a.GetName(sd)

    # The capture runs over a few flips. A frame ends where it is shown: a
    # present, or (a hidden window, a readback) the blit of the finished
    # picture. The last whole one is read.
    # (The capture's own last action is not one: it only closes the file.)
    body = [(i, a) for i, a in enumerate(actions) if name(a) != "End of Capture"]
    ends = [i for i, a in body if a.flags & rd.ActionFlags.Present]
    if not ends:
        ends = [i for i, a in body if "Blit" in name(a)]
    if ends:
        last = ends[-1]
        first = ends[-2] + 1 if len(ends) > 1 else 0
    else:
        first, last = 0, len(actions) - 1
    frame = actions[first:last + 1]
    lo, hi = frame[0].eventId, frame[-1].eventId
    draws = [a for a in frame if is_draw(a)]

    # The game's own name for each mesh call: the label the renderer puts
    # before it (native_pipeline.h's setDrawLabels), "rg f=.. n=.. .. src=..".
    # Target ids become the game's buffer addresses through the log's
    # [target-map] lines (the piece dump writes them at the mark).
    bases = {}
    try:
        for line in open(os.path.join(mark, "log.txt"), errors="replace"):
            if line.startswith("[target-map] base "):
                words = line.split()
                bases[words[5]] = words[2]
    except OSError:
        pass

    def buffer(tid):
        return "%s(buffer %s)" % (tid, bases[tid]) if tid in bases else tid

    labels = {}
    pending = None
    for a in frame:
        text = name(a)
        if text.startswith("rg f="):
            pending = text
        elif is_draw(a):
            if pending:
                labels[a.eventId] = pending
            pending = None

    def game_name(event, brief=False):
        label = labels.get(event)
        if not label:
            return ""
        head, _, source = label.partition(" src=")
        f = dict(w.split("=", 1) for w in head.split()[1:])
        reads = "untextured"
        if f.get("stex", "0") != "0":
            reads = "reads target %s" % buffer(f["stex"])
        elif f.get("tex", "0") != "0":
            reads = "texture %s" % f["tex"]
        kind = "object space, GPU matrices" if f.get("model") == "1" else (
            "window space, with a camera" if f.get("cam", "0") != "0" else "window space")
        if brief:
            return "%s [%s]" % (source, reads)
        return "%s | %s | %s | into %s | depth log frame %s draws %s%s" % (
            source, kind, reads, buffer(f.get("tgt", "?")), f.get("f"), f.get("n"),
            " | object %s" % f["obj"] if f.get("obj", "0") != "0" else "")

    say("mark     %s" % mark)
    say("commit   %s on %s (%s tracked files changed)" % (info.get("commit"), info.get("branch"), info.get("dirty")))
    say("at       flip %s, pad read %s, %s s" % (info.get("flip"), info.get("pad_read"), info.get("seconds")))
    say("capture  %s: %d actions, %d frame ends; reading events %d-%d (%d draws)" % (
        os.path.basename(captures[-1]), len(actions), len(ends), lo, hi, len(draws)))
    say()

    # Passes: runs of draws into the same targets, in order.
    say("== passes of the frame")
    passes = []
    for a in draws:
        colour = [o for o in a.outputs if o != null]
        key = (tuple(colour), a.depthOut)
        if not passes or passes[-1]["key"] != key:
            passes.append({"key": key, "draws": [], "indices": 0})
        passes[-1]["draws"].append(a)
        passes[-1]["indices"] += a.numIndices * max(1, a.numInstances)
    written = []
    for n, p in enumerate(passes):
        colour, depth = p["key"]
        sizes = ", ".join("%s %dx%d" % (short(c), textures[c].width, textures[c].height) if c in textures
                          else short(c) for c in colour)
        say("  pass %2d  events %5d-%-5d  %4d draws %8d indices  colour %s  depth %s" % (
            n, p["draws"][0].eventId, p["draws"][-1].eventId, len(p["draws"]), p["indices"], sizes or "-",
            short(depth) if depth != null else "-"))
        made = {}
        for a in p["draws"]:
            label = labels.get(a.eventId)
            if label:
                source = label.partition(" src=")[2]
                made[source] = made.get(source, 0) + 1
        if made:
            say("           " + ", ".join("%d %s" % (count, source) for source, count in
                                        sorted(made.items(), key=lambda item: -item[1])))
        for c in colour:
            if c not in written:
                written.append(c)
    say()

    # The picture: what the frame's end shows, else the last target drawn.
    end = frame[-1]
    shown = end.copySource if end.copySource != null else (written[-1] if written else null)
    ctrl.SetFrameEvent(hi, True)

    def save(rid, path):
        s = rd.TextureSave()
        s.resourceId = rid
        s.destType = rd.FileType.PNG
        s.mip = 0
        s.alpha = rd.AlphaMapping.Discard
        ctrl.SaveTexture(s, path)

    if shown != null:
        save(shown, os.path.join(report_dir, "frame.png"))
        say("frame.png        the picture (texture %s)" % short(shown))
    for c in written:
        if c != shown and c in textures:
            save(c, os.path.join(report_dir, "target_%s.png" % short(c)))
            say("target_%s.png  as the frame leaves it" % short(c))
    # Every frame of the capture, for a fault that comes and goes: the same
    # texture where each frame ends (the last is frame.png again).
    if shown != null and len(ends) > 1:
        for n, index in enumerate(ends):
            ctrl.SetFrameEvent(actions[index].eventId, True)
            save(shown, os.path.join(report_dir, "flip_%d.png" % n))
        ctrl.SetFrameEvent(hi, True)
        say("flip_0..%d.png   the picture at each frame end of the capture, oldest first" % (len(ends) - 1))
    say()

    # The point asked about, as a fraction of the picture.
    at = os.environ.get("BUGMARK_AT", "").split()
    uv = None
    how = ""
    if len(at) == 3 and at[0] == "uv":
        uv = (float(at[1]), float(at[2]))
        how = "given as a fraction of the picture"
    else:
        if len(at) == 2:
            px, py = float(at[0]), float(at[1])
        else:
            px, py = [float(v) for v in info.get("mouse_pixels", "-1 -1").split()]
        if px >= 0 and py >= 0:
            # Where the picture sits in the window: the present blit's
            # destination, or the viewport of the draw that shows it.
            rect = None
            try:
                if "Blit" in name(end):
                    chunk = sd.chunks[end.events[-1].chunkIndex]
                    region = chunk.FindChild("pRegions").GetChild(0)
                    d = region.FindChild("dstOffsets")
                    x0, y0 = d.GetChild(0).FindChild("x").AsInt(), d.GetChild(0).FindChild("y").AsInt()
                    x1, y1 = d.GetChild(1).FindChild("x").AsInt(), d.GetChild(1).FindChild("y").AsInt()
                    rect = (x0, y0, x1 - x0, y1 - y0)
                    how = "through the present blit's rectangle %s" % (rect,)
                elif draws:
                    ctrl.SetFrameEvent(draws[-1].eventId, True)
                    v = ctrl.GetPipelineState().GetViewport(0)
                    # A flipped viewport (negative height) covers the same pixels.
                    rect = (min(v.x, v.x + v.width), min(v.y, v.y + v.height), abs(v.width), abs(v.height))
                    how = "through the last draw's viewport %s" % (rect,)
                    ctrl.SetFrameEvent(hi, True)
            except Exception:
                rect = None
            if not rect or rect[2] == 0 or rect[3] == 0:
                w, h = [float(v) for v in info.get("window_pixels", "1 1").split()]
                rect = (0.0, 0.0, w, h)
                how = "taking the picture to fill the window (%dx%d): check it" % (w, h)
            uv = ((px - rect[0]) / rect[2], (py - rect[1]) / rect[3])
    if uv is None:
        say("== no point given: no mouse over the window at the mark. Give one with --at X Y or --uv U V.")
        ctrl.Shutdown()
        cap.Shutdown()
        return
    if not (0.0 <= uv[0] < 1.0 and 0.0 <= uv[1] < 1.0):
        say("== the point is outside the picture: uv %.3f %.3f (%s)" % (uv[0], uv[1], how))
        ctrl.Shutdown()
        cap.Shutdown()
        return
    say("== the point: %.4f %.4f of the picture, %s" % (uv[0], uv[1], how))
    say("   Each target the frame draws into is asked what touched that point, in order.")
    say()

    seen_draws = []
    for c in written:
        if c not in textures:
            continue
        t = textures[c]
        x, y = int(uv[0] * t.width), int(uv[1] * t.height)
        history = ctrl.PixelHistory(c, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        mods = [m for m in history if lo <= m.eventId <= hi]
        say("-- target %s (%dx%d) at %d,%d: %d touches" % (short(c), t.width, t.height, x, y, len(mods)))
        by_event = {a.eventId: a for a in frame}
        for m in mods:
            a = by_event.get(m.eventId)
            what = "%s %d indices" % (name(a), a.numIndices) if a else "?"
            if a and game_name(m.eventId):
                what = "%s (%d indices)" % (game_name(m.eventId, True), a.numIndices)
            if m.Passed():
                verdict = "WROTE   %s -> %s  shader %s  depth %.6f -> %.6f" % (
                    rgba(m.preMod), rgba(m.postMod), rgba(m.shaderOut), m.preMod.depth, m.postMod.depth)
            else:
                why = [label for label, flag in (
                    ("depth test", m.depthTestFailed), ("backface", m.backfaceCulled),
                    ("shader discard", m.shaderDiscarded), ("scissor", m.scissorClipped),
                    ("depth clip", m.depthClipped), ("view clip", m.viewClipped),
                    ("stencil", m.stencilTestFailed), ("sample mask", m.sampleMasked),
                    ("depth bounds", m.depthBoundsFailed)) if flag]
                verdict = "lost to %s  (its depth %.6f, there %.6f)" % (
                    ", ".join(why) or "?", m.shaderOut.depth, m.preMod.depth)
            say("   event %5d prim %5d  %-52s %s" % (m.eventId, m.primitiveID, what, verdict))
            if a and is_draw(a) and m.eventId not in seen_draws:
                seen_draws.append(m.eventId)
        say()

    # Each of those draws: its depth and blend state and the textures it reads.
    say("== the draws that touched the point")
    saved = set()
    for event in seen_draws[:60]:
        ctrl.SetFrameEvent(event, True)
        line = "   event %5d " % event
        try:
            vk = ctrl.GetVulkanPipelineState()
            ds = vk.depthStencil
            line += " depth test %s %s write %s " % (
                "on" if ds.depthTestEnable else "off", enum(rd.CompareFunction, ds.depthFunction),
                "on" if ds.depthWriteEnable else "off")
            blend = vk.colorBlend.blends[0]
            if blend.enabled:
                line += " blend %s*src %s %s*dst " % (
                    enum(rd.BlendMultiplier, blend.colorBlend.source),
                    enum(rd.BlendOperation, blend.colorBlend.operation),
                    enum(rd.BlendMultiplier, blend.colorBlend.destination))
            else:
                line += " blend off "
        except Exception as error:
            line += " (state: %s) " % error
        reads = []
        try:
            for used in ctrl.GetPipelineState().GetReadOnlyResources(rd.ShaderStage.Pixel):
                rid = used.descriptor.resource
                if rid != null and rid in textures and rid not in reads:
                    reads.append(rid)
        except Exception as error:
            line += " (textures: %s) " % error
        line += " reads " + (", ".join("%s %dx%d" % (short(r), textures[r].width, textures[r].height)
                                       for r in reads) or "-")
        say(line)
        if game_name(event):
            say("               %s" % game_name(event))
        for r in reads:
            if r not in saved and r not in written and len(saved) < 40:
                saved.add(r)
                save(r, os.path.join(report_dir, "tex_%s.png" % short(r)))
    if saved:
        say()
        say("tex_N.png: the textures those draws read, as they were at the draw.")
    ctrl.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    say(traceback.format_exc())
out.close()
os._exit(0)
