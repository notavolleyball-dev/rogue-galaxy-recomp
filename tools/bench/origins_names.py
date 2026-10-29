"""Names the call sites in an RG_DRAW_ORIGINS report (src/debug/draw_origins.h)
and sums them by function.

usage: origins_names.py REPORT [--sites]   (from the repository root)

Prints one row per function (or per call site with --sites), in the
report's columns, per frame. A .seq file beside the report (RG_DRAW_ORIGINS_SEQ)
is rewritten as REPORT.seq.named with the names in place.
"""
import bisect
import csv
import os
import re
import sys

MAPS = "config/SCUS_974.90/functions_main.csv"

COLUMNS = ["packets", "prims", "sprites", "spriteKpx", "tris", "textured", "targetReads", "feedback",
           "reinterp", "uploads", "uploadKB", "readbacks", "copies", "meshes"]


def load_symbols():
    syms = []
    with open(MAPS, newline="") as f:
        reader = csv.reader(f)
        next(reader)
        for name, start, end, _ in reader:
            syms.append((int(start, 16), int(end, 16), name))
    syms.sort()
    return syms, [s[0] for s in syms]


def name_of(address, syms, starts):
    i = bisect.bisect_right(starts, address) - 1
    if i >= 0 and syms[i][0] <= address < syms[i][1]:
        return syms[i][2], address - syms[i][0]
    return f"?{address:06x}", 0


def parse(path):
    rows = []
    header = ""
    for line in open(path):
        if line.startswith("#"):
            if not header:
                header = line.strip()
            continue
        parts = [p.strip() for p in line.rstrip("\n").split("|")]
        if len(parts) < 6:
            continue
        numbers = " ".join(parts[1:5]).split()
        rows.append((parts[0], [float(n) for n in numbers], parts[5], parts[6] if len(parts) > 6 else ""))
    return header, rows


def merge_counts(a, b):
    counts = {}
    for text in (a, b):
        tokens = text.split()
        for i in range(0, len(tokens) - 1, 2):
            if ":" in tokens[i]:
                counts[tokens[i]] = counts.get(tokens[i], 0.0) + float(tokens[i + 1])
    return counts


def show_counts(counts):
    top = sorted(counts.items(), key=lambda kv: -kv[1])[:5]
    return " ".join(f"{k} {v:.0f}" for k, v in top)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    path = sys.argv[1]
    sites = "--sites" in sys.argv
    syms, starts = load_symbols()
    header, rows = parse(path)
    groups = {}
    for key, numbers, targets, textures in rows:
        m = re.match(r"fn ([0-9a-f]+)$", key)
        if m:
            name, offset = name_of(int(m.group(1), 16), syms, starts)
            label = f"{name}+{offset:x}" if sites else name
        else:
            label = key
        group = groups.setdefault(label, [[0.0] * len(COLUMNS), {}, {}, 0])
        group[0] = [a + b for a, b in zip(group[0], numbers)]
        for k, v in merge_counts(targets, "").items():
            group[1][k] = group[1].get(k, 0.0) + v
        for k, v in merge_counts(textures, "").items():
            group[2][k] = group[2].get(k, 0.0) + v
        group[3] += 1
    print(header)
    print(f"{'origin':48s} {'prims':>7s} {'sprites':>7s} {'Kpx':>7s} {'tris':>6s} {'tgtRd':>6s} {'feedbk':>6s} "
          f"{'reint':>6s} {'upKB':>6s} {'rdbk':>5s} {'copy':>5s} | targets | textures")
    for label, (n, targets, textures, count) in sorted(groups.items(), key=lambda kv: -kv[1][0][1]):
        if n[1] < 0.05 and n[10] < 0.5:
            continue
        print(f"{label[:48]:48s} {n[1]:7.1f} {n[2]:7.1f} {n[3]:7.1f} {n[4]:6.1f} {n[6]:6.1f} {n[7]:6.1f} "
              f"{n[8]:6.1f} {n[10]:6.1f} {n[11]:5.2f} {n[12]:5.2f} | {show_counts(targets)} | {show_counts(textures)}")
    seq = path + ".seq"
    if os.path.exists(seq):
        with open(seq + ".named", "w") as out:
            for line in open(seq):
                m = re.match(r"fn ([0-9a-f]+)", line)
                if m:
                    name, offset = name_of(int(m.group(1), 16), syms, starts)
                    line = f"{name}+{offset:x}".ljust(44) + line[len(m.group(0)):].lstrip()
                out.write(line)


if __name__ == "__main__":
    main()
