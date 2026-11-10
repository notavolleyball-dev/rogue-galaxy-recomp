#!/usr/bin/env python3
"""Read a bug mark (F8 in the game; src/debug/bug_mark.h) and say what drew
the pixel the mouse was on.

    tools/bugmark/bugmark.py [MARK] [--at X Y | --uv U V]

MARK is a folder under build/run/marks (default: the newest). Writes
MARK/report/ and prints report.txt:
    report.txt    the frame's passes, then for the point: every draw that
                  touched it in every target, in order, whether it wrote or
                  what it lost to (depth test, discard, backface...), the
                  colours and depths before and after, and each such draw's
                  depth and blend state and textures
    frame.png     the picture of that frame; target_N.png the other targets
    tex_N.png     the textures those draws read
--at takes window pixels (as the mouse is recorded), --uv a fraction of the
picture from its top left. Without either the mark's mouse position is used.

The capture is replayed by RenderDoc (qrenderdoc, headless). The game writes
one only when started under it (ENABLE_VULKAN_RENDERDOC_CAPTURE=1, with a
layer that has Wayland support on Wayland; tools/run/play.sh rdoc). Without a capture the folder still has info.txt, the
depth log (tools/depthlog.py) and the log around the mark.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mark", nargs="?")
    parser.add_argument("--at", nargs=2, metavar=("X", "Y"))
    parser.add_argument("--uv", nargs=2, metavar=("U", "V"))
    args = parser.parse_args()

    mark = args.mark
    if not mark:
        marks = sorted(d for d in glob.glob("build/run/marks/*") if os.path.isdir(d))
        if not marks:
            sys.exit("no marks under build/run/marks")
        mark = marks[-1]
    mark = os.path.abspath(mark)
    if not os.path.isfile(os.path.join(mark, "info.txt")):
        sys.exit("%s is not a bug mark (no info.txt)" % mark)
    if not shutil.which("qrenderdoc"):
        sys.exit("qrenderdoc is not installed (the renderdoc package)")

    env = dict(os.environ, BUGMARK_DIR=mark, QT_QPA_PLATFORM="offscreen")
    env["BUGMARK_AT"] = "uv %s %s" % tuple(args.uv) if args.uv else " ".join(args.at or [])
    report = os.path.join(mark, "report", "report.txt")
    if os.path.exists(report):
        os.remove(report)
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "rdoc_read.py")
    try:
        subprocess.run(["qrenderdoc", "--python", script], env=env, timeout=300,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        # Its first start asks about analytics in a window nobody sees here.
        sys.exit("qrenderdoc did not finish. If it has never been started, start it once by hand "
                 "and answer its first question.")
    if not os.path.exists(report):
        sys.exit("qrenderdoc wrote no report")
    sys.stdout.write(open(report).read())
    print("\nfiles in %s" % os.path.dirname(report))


if __name__ == "__main__":
    main()
