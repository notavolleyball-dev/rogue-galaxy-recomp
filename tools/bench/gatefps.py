"""Frame rate and VIF1 cost at the boss gate: gatefps.py NAME...

A gate bench run (`GATE=1 tools/bench/walk.sh NAME 3 60`) loads the user's
File 03 beside a boss gate and stands there, where the VIF1 thread (the VU1
programs) is the limit. The field intervals are those whose `[gfx]` line
counts more than 5000 GIF packets a frame (the load screen and the menus
have a few hundred). The scene varies between runs of the 60 fps mode
(40-54 fps with one build), so besides the median frame rate this prints
the VIF1 thread's milliseconds a frame (its busy share of the wall over the
frame rate), which moves much less, and the GIF stage's.
"""
import re
import statistics
import sys


def main() -> int:
    for name in sys.argv[1:]:
        fps, vif1, gif = [], [], []
        busy = None
        for line in open(f"build/run/{name}.log", encoding="utf-8", errors="replace"):
            m = re.search(r"vif1 busy=(\d+)%(?: gif busy=(\d+)%)?", line)
            if m:
                busy = (int(m.group(1)), int(m.group(2)) if m.group(2) else None)
                continue
            m = re.match(r"\[gfx\] ([\d.]+) fps .*gif packets=(\d+)", line)
            if m and int(m.group(2)) // 60 > 5000 and busy:
                rate = float(m.group(1))
                fps.append(rate)
                vif1.append(busy[0] * 10.0 / rate)
                if busy[1] is not None:
                    gif.append(busy[1] * 10.0 / rate)
        if not fps:
            print(f"{name}: no field interval")
            continue
        text = (f"{name}: {len(fps)} field intervals, median {statistics.median(fps):.1f} fps "
                f"(min {min(fps):.1f}, max {max(fps):.1f}); VIF1 {statistics.median(vif1):.1f} ms a frame")
        if gif:
            text += f", GIF stage {statistics.median(gif):.1f} ms"
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
