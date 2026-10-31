#!/usr/bin/env python3
"""pixdiff.py A B: the display buffers of two logged frames (build/run/A.txt.vram,
B's) compared pixel by pixel: how many differ, a histogram of the largest
channel difference, and the bounding box and a few of the pixels that differ."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
W, H = 512, 448
def offsets():
    BLOCKS = [[0, 1, 4, 5, 16, 17, 20, 21], [2, 3, 6, 7, 18, 19, 22, 23],
              [8, 9, 12, 13, 24, 25, 28, 29], [10, 11, 14, 15, 26, 27, 30, 31]]
    COLUMN = [[0, 1, 4, 5, 8, 9, 12, 13], [2, 3, 6, 7, 10, 11, 14, 15]]
    out = []
    for y in range(H):
        for x in range(W):
            page = (y // 32) * 8 + x // 64
            px, py = x % 64, y % 32
            block = BLOCKS[py // 8][px // 8]
            bx, by = px % 8, py % 8
            out.append((page * 2048 + block * 64 + (by // 2) * 16 + COLUMN[by % 2][bx]) * 4)
    return out
where = offsets()
va = open(f"build/run/{sys.argv[1]}.txt.vram", "rb").read()
vb = open(f"build/run/{sys.argv[2]}.txt.vram", "rb").read()
hist = {}
xs, ys, shown = [], [], []
for i, o in enumerate(where):
    a, b = va[o:o + 4], vb[o:o + 4]
    if a == b:
        continue
    d = max(abs(a[k] - b[k]) for k in range(3))
    da = abs(a[3] - b[3])
    key = (min(d, 64) if d < 64 else '64+', 'rgb' if d else 'alpha')
    hist[key] = hist.get(key, 0) + 1
    x, y = i % W, i // W
    if d:
        xs.append(x); ys.append(y)
        if len(shown) < 12 and d > 8:
            shown.append((x, y, a.hex(), b.hex()))
print(f"{sum(hist.values())} pixels differ")
for k in sorted(hist, key=str):
    print("  max channel diff", k, hist[k])
if xs:
    print("  colour differences within x %d-%d, y %d-%d" % (min(xs), max(xs), min(ys), max(ys)))
for s in shown:
    print("  ", s)
