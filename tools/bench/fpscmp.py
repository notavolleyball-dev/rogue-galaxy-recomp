"""Per-interval fps of a walk's log (DQ8_GFX_STATS_EVERY lines), aligned on
the census's "frame logged" marker: the N intervals before it are the walk
through Salgin's streets.

usage: fpscmp.py LOG [LOG...] [-n N]
"""
import re
import sys

args = []
n = 5
window = None  # (start, end) in intervals after the marker: the long walk's pacing
rest = sys.argv[1:]
while rest:
    a = rest.pop(0)
    if a == "-n":
        n = int(rest.pop(0))
    elif a == "-w":
        start, end = rest.pop(0).split(":")
        window = (int(start), int(end))
    else:
        args.append(a)
fps_re = re.compile(r"^\[gfx\] ([\d.]+) fps \(backend (\d+)% of wall\)")
res_re = re.compile(r"readback wait ([\d.]+) ms/frame")

for log in args:
    intervals = []   # (fps, backend%, readback ms)
    marker = None
    pending = None
    with open(log, errors="replace") as f:
        for line in f:
            m = fps_re.match(line)
            if m:
                pending = [float(m.group(1)), int(m.group(2)), None]
                intervals.append(pending)
                continue
            m = res_re.search(line)
            if m and pending is not None:
                pending[2] = float(m.group(1))
                continue
            if "frame logged" in line:
                marker = len(intervals)
    print(f"{log}: {len(intervals)} intervals, frame logged after interval {marker}")
    if marker is None:
        marker = len(intervals)
    last = intervals[max(0, marker - n):marker]
    if last:
        fps = sum(w[0] for w in last) / len(last)
        backend = sum(w[1] for w in last) / len(last)
        rb = [w[2] for w in last if w[2] is not None]
        rbs = f", readback {sum(rb) / len(rb):.2f} ms/frame" if rb else ""
        print(f"  last {len(last)} intervals before the marker: {fps:.1f} fps, backend {backend:.0f}%{rbs}")
        print("  " + " ".join(f"{w[0]:.1f}" for w in last))
    if window is not None and marker is not None:
        start, end = window
        after = intervals[marker + start:marker + end]
        if after:
            fps = sum(w[0] for w in after) / len(after)
            backend = sum(w[1] for w in after) / len(after)
            print(f"  intervals {start}..{end} after the marker ({len(after)}): {fps:.1f} fps, backend {backend:.0f}%")
            print("  " + " ".join(f"{w[0]:.1f}" for w in after))
