"""Which GS states a set of frame logs uses that a GPU blend cannot express directly.

  gsstates.py LOG...    (RG_GS_FRAMELOG or F7 capture .txt files)

Per state, the batches using it, how many logs have it and an example batch.
Without framebuffer fetch the renderer blends in the shader from a snapshot
for the destination alpha test, destination-alpha factors and factors past
one ("destination reads" in the stats line); FBA is in its shader; the rest
are approximations worth a look when a picture looks wrong.
"""
import collections
import sys

FULL_MASKS = {0x0, 0xff000000, 0x00ffffff, 0xffffffff}


def states(m, vertex_alpha_max):
    test = int(m["test"], 16)
    alpha = int(m["alpha"], 16)
    abe = m["abe"] == "1"
    c = (alpha >> 4) & 3
    fix = (alpha >> 32) & 0xff
    fbmsk = int(m["fbmsk"], 16)
    out = []
    if (test & 1) and ((test >> 12) & 3):
        out.append("alpha-fail mode %d" % ((test >> 12) & 3))
    if test & (1 << 14):
        out.append("destination alpha test")
    if abe and c == 1:
        out.append("blend factor = destination alpha")
    if abe and m.get("pabe") == "1":
        out.append("PABE")
    if abe and m.get("colclamp") == "0":
        out.append("colour clamp off (wrap)")
    if fbmsk not in FULL_MASKS:
        partial = any(((fbmsk >> s) & 0xff) not in (0, 0xff) for s in (0, 8, 16, 24))
        out.append("partial bit mask" if partial else "byte mask %08x" % fbmsk)
    if abe and c == 2 and fix > 0x80:
        out.append("FIX above 0x80")
    if abe and c == 0 and vertex_alpha_max > 0x80 and (m.get("tme") == "0" or m.get("tcc") == "0"):
        out.append("source alpha above 0x80")
    if m.get("dthe") == "1":
        out.append("dither")
    if m.get("fba") == "1":
        out.append("FBA")
    if int(m["psm"]) in (2, 10) and abe:
        out.append("blend on CT16")
    return out


def main():
    tally = collections.Counter()
    logs = collections.defaultdict(set)
    example = {}
    for path in sys.argv[1:]:
        with open(path, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                if not line.startswith("B "):
                    continue
                parts = line.split(" | ")
                head = parts[0].split()
                m = dict(kv.split("=", 1) for kv in head[2:] if "=" in kv)
                vertex_alpha = 0
                for vertex in parts[1:]:
                    fields = vertex.split()
                    if len(fields) > 3:
                        try:
                            vertex_alpha = max(vertex_alpha, int(fields[3], 16) & 0xff)
                        except ValueError:
                            pass
                for state in states(m, vertex_alpha):
                    tally[state] += 1
                    logs[state].add(path.replace("\\", "/").split("/")[-1])
                    example.setdefault(state, head[1] + " " + " ".join(
                        k + "=" + m.get(k, "?") for k in ("prim", "tme", "abe", "alpha", "test", "fbp", "psm",
                                                         "fbmsk", "tbp", "tpsm", "colclamp", "pabe", "fba")))
    for state, n in tally.most_common():
        names = sorted(logs[state])
        print("%-34s %8d batches in %3d logs, e.g. %s" % (state, n, len(names), ", ".join(names[:4])))
        print("    B " + example[state])


main()
