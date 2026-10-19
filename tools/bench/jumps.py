"""Summarise [jump] lines: one row per arc (a run of consecutive moves)."""
import re, sys
pat = re.compile(r"\[jump\]\s+([\d.]+) s move (\d+): y\s+([-\d.]+) vy\s+([-\d.]+) dy\s+([-\d.]+) xz \(([-\d.]+), ([-\d.]+)\) step ([\d.]+) mode (-?\d+) move (-?\d+)/(-?\d+)(.*)")
rows = []
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    m = pat.search(line)
    if m:
        t, mv, y, vy, dy, x, z, step, mode, mid, ph, fl = m.groups()
        rows.append(dict(t=float(t), move=int(mv), y=float(y), vy=float(vy), dy=float(dy), x=float(x), z=float(z),
                         step=float(step), mode=int(mode), mid=int(mid), ph=int(ph), flags=fl.strip()))
arcs = []
cur = []
for r in rows:
    if cur and r["move"] != cur[-1]["move"] + 1:
        arcs.append(cur); cur = []
    cur.append(r)
if cur: arcs.append(cur)
print(f"{len(rows)} lines, {len(arcs)} arcs")
for a in arcs:
    y0 = a[0]["y"]; peak = max(r["y"] for r in a)
    ipeak = max(range(len(a)), key=lambda i: a[i]["y"])
    dist = ((a[-1]["x"] - a[0]["x"]) ** 2 + (a[-1]["z"] - a[0]["z"]) ** 2) ** 0.5
    active = [r for r in a if r["vy"] != 0 or r["dy"] != 0 or r["ph"] != 0 or r["flags"]]
    dur = (active[-1]["t"] - active[0]["t"]) if active else 0
    modes = sorted(set(r["mode"] for r in a)); phases = sorted(set(r["ph"] for r in a))
    flags = sorted(set(r["flags"] for r in a if r["flags"]))
    print(f"arc at {a[0]['t']:8.3f} s: {len(a)} moves ({len(active)} active), {dur:5.3f} s, rise {peak - y0:6.2f} "
          f"(peak after {a[ipeak]['t'] - a[0]['t']:.3f} s), horizontal {dist:5.2f}, modes {modes} phases {phases} flags {flags}")
if "-v" in sys.argv:
    for a in arcs:
        print("---")
        for r in a:
            print(f"{r['t']:8.3f} y {r['y']:8.3f} vy {r['vy']:7.3f} dy {r['dy']:7.3f} step {r['step']:.2f} mode {r['mode']} move {r['mid']}/{r['ph']} {r['flags']}")
