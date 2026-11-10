#!/usr/bin/env python3
"""Read a native-renderer depth log (build/run/depthlog-<frame>.txt) and say
where the native renderer's depth test comes out differently from the GS's.

The renderer writes the log after `touch build/run/depth-log-now`: per frame,
each pass (P: target, depth target, cleared or loaded) and each draw as
drawn (D: material, camera, depth pull, the program's depthScale/depthMax;
T: its triangles, per corner x y in target pixels, the depth the GPU gets
(reversed float, nearer is greater) and the integer Z the GS would have got).

1. Depth error per draw: at every corner, our depth in GS units
   (V22 * z/w + V32, the pull taken out) minus the GS's Z. A draw the
   program scales or caps (depthScale, depthMax) shows as an offset here.
2. Replays: at points inside every depth-tested draw's triangles, the depth
   buffer is replayed twice, from our depths and from the GS's Z (every
   earlier depth-writing triangle on that depth target since its clear, in
   order, under its own test). Points where the two disagree are listed by
   draw and by what beat it, with both margins.

Texture alpha is not known here: a writer with an alpha test may not have
written at a point (its `at` says so); both replays share that error.

Usage: tools/depthlog.py build/run/depthlog-N.txt [--top 25]
"""
import argparse
import bisect
import collections
import statistics
import sys

TILE = 32
TESTS = {0: "always", 1: "never", 2: "<", 3: "<=", 4: "==", 5: ">=", 6: ">", 7: "!="}


def passes(test, value, stored):
    if test == 0:
        return True
    if test == 1:
        return False
    if test == 2:
        return value < stored
    if test == 3:
        return value <= stored
    if test == 4:
        return value == stored
    if test == 5:
        return value >= stored
    if test == 6:
        return value > stored
    return value != stored


def parse_kv(line):
    out = {}
    for part in line.split()[1:]:
        k, _, v = part.partition("=")
        out[k] = v
    return out


class Draw:
    __slots__ = ("n", "kv", "tris", "gtris", "dt", "zt", "zw", "bl", "model", "v22", "v32", "pull", "key",
                 "zs", "zmax")

    def __init__(self, kv, dt):
        self.kv = kv
        self.n = int(kv["n"])
        self.dt = dt  # the pass's depth target (0: none attached)
        self.zt = int(kv["zt"])
        self.zw = int(kv["zw"])
        self.bl = int(kv["bl"])
        self.model = int(kv["model"])
        self.v22 = float(kv["v22"])
        self.v32 = float(kv["v32"])
        self.pull = float(kv["pull"])
        self.zs = kv.get("zs", "?")
        self.zmax = kv.get("zmax", "?")
        self.tris = []   # (x0 y0 d0 x1 y1 d1 x2 y2 d2), our depth; None if clipped
        self.gtris = []  # the same with the GS's Z; None if unknown
        # Identity across frames: object-space vertices for a 3D draw; a
        # window-space draw's change with the camera, so its source,
        # texture and size instead.
        if self.model:
            self.key = "3d:" + kv["geo"]
        else:
            self.key = "2d:src%s:tex%s:stex%s:tris%s" % (kv["src"], kv["tex"], kv["stex"], kv["tris"])

    def desc(self):
        k = self.kv
        return ("%s n=%s cam=%s zt=%s zw=%s bl=%s(%s,%s,%s) at=%s/%s af=%s tex=%s stex=%s src=%s gpass=%s "
                "zs=%s zmax=%s pull=%s tris=%s" % (
                    "3d" if self.model else "2d", k["n"], k["cam"], TESTS.get(self.zt, k["zt"]), k["zw"], k["bl"],
                    k["sf"], k["df"], k["op"], k["at"], k["ar"], k["af"], k["tex"], k["stex"], k["src"],
                    k["gpass"], self.zs, self.zmax, k["pull"], k["tris"]))


def read_frames(path):
    frames = []
    frame = None
    passinfo = None
    pending = None
    with open(path) as f:
        for line in f:
            tag = line[:1]
            if tag == "F":
                frame = {"kv": parse_kv(line), "draws": [], "clears": []}
                frames.append(frame)
            elif tag == "P":
                passinfo = parse_kv(line)
                if passinfo["depth"] == "clear":
                    frame["clears"].append((int(passinfo["dt"]), int(passinfo["first"])))
            elif tag == "D":
                kv = parse_kv(line)
                dt = int(passinfo["dt"]) if passinfo and kv["att"] == "1" else 0
                pending = Draw(kv, dt)
                frame["draws"].append(pending)
            elif tag == "T" and pending is not None:
                vals = line.split()[1:]
                per = 4 if "gs" in pending.kv else 3
                stride = per * 3
                for i in range(0, len(vals) - stride + 1, stride):
                    t = vals[i:i + stride]
                    corners = [t[c * per:(c + 1) * per] for c in range(3)]
                    if any(c[0] == "nan" for c in corners):
                        pending.tris.append(None)
                        pending.gtris.append(None)
                        continue
                    pending.tris.append(tuple(float(v) for c in corners for v in c[:3]))
                    if per == 4 and all(c[3] != "nan" for c in corners):
                        pending.gtris.append(tuple(float(v) for c in corners for v in (c[0], c[1], c[3])))
                    else:
                        pending.gtris.append(None)
                pending = None
    return frames


def bary(tri, px, py):
    x0, y0, z0, x1, y1, z1, x2, y2, z2 = tri
    d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
    if abs(d) < 1e-12:
        return None
    a = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / d
    b = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / d
    c = 1.0 - a - b
    if a < 0 or b < 0 or c < 0:
        return None
    # Depth after the divide (and the GS's Z) is linear in screen space.
    return a * z0 + b * z1 + c * z2


def samples(tri):
    x0, y0, _, x1, y1, _, x2, y2, _ = tri
    area = abs((x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)) * 0.5
    if area < 0.25:
        return []
    cx, cy = (x0 + x1 + x2) / 3.0, (y0 + y1 + y2) / 3.0
    pts = [(cx, cy)]
    for vx, vy in ((x0, y0), (x1, y1), (x2, y2)):
        pts.append((vx * 0.7 + cx * 0.3, vy * 0.7 + cy * 0.3))
    return pts


def ours_in_gs(d, depth):
    # depth = (1 - z/w) / 2 + pull, so z/w = 1 - 2 (depth - pull); Z = V22 z/w + V32.
    return d.v22 * (1.0 - 2.0 * (depth - d.pull)) + d.v32


def depth_errors(frames):
    """Per draw key: corner errors (ours in GS units - GS Z)."""
    errs = collections.defaultdict(list)
    first = {}
    for fr in frames:
        for d in fr["draws"]:
            if not d.v22:
                continue
            for t, g in zip(d.tris, d.gtris):
                if t is None or g is None:
                    continue
                for c in range(3):
                    errs[d.key].append(ours_in_gs(d, t[c * 3 + 2]) - g[c * 3 + 2])
            first.setdefault(d.key, d)
    return errs, first


def replay(frame):
    """Per depth-tested draw: points where our test and the GS's differ."""
    draws = frame["draws"]
    clears = collections.defaultdict(list)
    for dt, first in frame["clears"]:
        clears[dt].append(first)
    bins = collections.defaultdict(lambda: collections.defaultdict(list))
    for d in draws:
        if not d.dt or not d.zw:
            continue
        b = bins[d.dt]
        for t, g in zip(d.tris, d.gtris):
            if t is None:
                continue
            xs, ys = (t[0], t[3], t[6]), (t[1], t[4], t[7])
            for tx in range(int(max(min(xs), -64) // TILE), int(min(max(xs), 4096) // TILE) + 1):
                for ty in range(int(max(min(ys), -64) // TILE), int(min(max(ys), 4096) // TILE) + 1):
                    b[(tx, ty)].append((d.n, t, g, d))
    out = []
    for d in draws:
        if not d.dt or d.zt in (0, 1):
            continue
        cl = clears.get(d.dt, [])
        i = bisect.bisect_right(cl, d.n) - 1
        since = cl[i] if i >= 0 else -1
        b = bins.get(d.dt, {})
        tested, differ = 0, []
        for t, g in zip(d.tris, d.gtris):
            if t is None or g is None:
                continue
            for px, py in samples(t):
                own = bary(t, px, py)
                gown = bary(g, px, py)
                if own is None or gown is None:
                    continue
                tested += 1
                stored, last, gstored, glast = 0.0, None, 0.0, None
                for order, wt, wg, wd in b.get((int(px // TILE), int(py // TILE)), ()):
                    if order >= d.n:
                        break
                    if order < since:
                        continue
                    z = bary(wt, px, py)
                    if z is not None and 0.0 <= z <= 1.0 and passes(wd.zt, z, stored):
                        stored, last = z, wd
                    if wg is not None:
                        gz = bary(wg, px, py)
                        if gz is not None and passes(wd.zt, gz, gstored):
                            gstored, glast = gz, wd
                ours = passes(d.zt, own, stored)
                gs = passes(d.zt, gown, gstored)
                if ours != gs:
                    differ.append((px, py, ours, ours_in_gs(d, own) - ours_in_gs(d, stored) if last else None,
                                   gown - gstored if glast else None, last, glast))
        out.append((d, tested, differ))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()

    frames = read_frames(args.log)
    print("%d frames in %s" % (len(frames), args.log))
    if not frames:
        return
    if not any(any(g is not None for g in d.gtris) for fr in frames for d in fr["draws"]):
        print("no GS Z in this log (an older build): nothing to compare")
        return

    # Program depth settings in use.
    settings = collections.Counter()
    for fr in frames:
        for d in fr["draws"]:
            settings[(d.model, d.zs, d.zmax, d.kv.get("gs"))] += 1
    print("\n== draws by depthScale / depthMax (all frames) ==")
    for (model, zs, zmax, gs), c in settings.most_common():
        print("  %s zs=%s zmax=%s gs-known=%s: %d" % ("3d" if model else "2d", zs, zmax, gs, c))

    # 1. Depth error per draw.
    errs, first = depth_errors(frames)
    print("\n== depth error per draw: ours (GS units, pull removed) - GS Z ==")
    allerr = [e for v in errs.values() for e in v]
    if allerr:
        print("all corners: %d, median %.3f, |err| > 1: %d, > 16: %d, > 1000: %d" % (
            len(allerr), statistics.median(allerr), sum(abs(e) > 1 for e in allerr),
            sum(abs(e) > 16 for e in allerr), sum(abs(e) > 1000 for e in allerr)))
    groups = collections.defaultdict(list)
    for key, v in errs.items():
        d = first[key]
        groups[("3d" if d.model else "2d", d.zs, d.zmax, d.kv["src"])].extend(v)
    print("by kind, depthScale, depthMax, source:")
    for g, v in sorted(groups.items(), key=lambda kv: -max(abs(e) for e in kv[1])):
        print("  %s zs=%s zmax=%s src=%s: %d corners, median %.3f, min %.3f, max %.3f" % (
            g[0], g[1], g[2], g[3], len(v), statistics.median(v), min(v), max(v)))
    worst = sorted(errs.items(), key=lambda kv: -abs(statistics.median(kv[1])))
    print("draws with the largest median error:")
    for key, v in worst[:args.top]:
        print("  %s median %.3f min %.3f max %.3f\n    %s" % (
            key, statistics.median(v), min(v), max(v), first[key].desc()))

    # 2. Replays.
    print("\n== where our depth test and the GS's disagree ==")
    by_layer = collections.defaultdict(lambda: collections.defaultdict(lambda: [0, 0]))  # key -> frame -> [tested, differ]
    pairs = collections.defaultdict(list)
    descs = {}
    total_tested = total_differ = 0
    for fi, fr in enumerate(frames):
        for d, tested, differ in replay(fr):
            descs.setdefault(d.key, d)
            by_layer[d.key][fi][0] += tested
            by_layer[d.key][fi][1] += len(differ)
            total_tested += tested
            total_differ += len(differ)
            for px, py, ours, om, gm, last, glast in differ:
                okey = (last.key if last else "-") + " / GS: " + (glast.key if glast else "-")
                pairs[(d.key, "hidden here, shown on GS" if not ours else "shown here, hidden on GS", okey)].append(
                    (fi, px, py, om, gm, last, glast))
    print("points tested %d, disagreeing %d" % (total_tested, total_differ))
    ranked = sorted(pairs.items(), key=lambda kv: -len(kv[1]))
    for (lkey, what, okey), hits in ranked[:args.top]:
        fs = sorted(set(h[0] for h in hits))
        om = [h[3] for h in hits if h[3] is not None]
        gm = [h[4] for h in hits if h[4] is not None]
        print("\n%s: %s, %d points over %d of %d frames" % (lkey, what, len(hits), len(fs), len(frames)))
        print("  layer:     %s" % descs[lkey].desc())
        if hits[0][5]:
            print("  ours beat by:  %s" % hits[0][5].desc())
        if hits[0][6]:
            print("  GS's last:     %s" % hits[0][6].desc())
        if om:
            print("  our margin (layer - stored, GS units): median %.3f min %.3f max %.3f" % (
                statistics.median(om), min(om), max(om)))
        if gm:
            print("  GS margin  (layer - stored, Z units):  median %.3f min %.3f max %.3f" % (
                statistics.median(gm), min(gm), max(gm)))
        xs = [h[1] for h in hits]
        ys = [h[2] for h in hits]
        print("  at x %.0f-%.0f y %.0f-%.0f" % (min(xs), max(xs), min(ys), max(ys)))
        per = by_layer[lkey]
        print("  per frame (differing/tested): " + " ".join(
            "-" if fi not in per else "%d/%d" % (per[fi][1], per[fi][0]) for fi in range(len(frames))))


if __name__ == "__main__":
    sys.exit(main())
