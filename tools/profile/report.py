"""Per-thread, per-function report from an RG_PROFILE sample file.

    python tools/profile/report.py build/run/profile.txt build/game/src/roguegalaxy.exe [--top 25]

Symbols come from `llvm-nm` (the executable's COFF symbol table), so the
recompiled game functions show up under their map names.
"""

from __future__ import annotations

import argparse
import bisect
import collections
import re
import shutil
import subprocess
import sys


def load_symbols(exe: str) -> tuple[list[int], list[str], int]:
    nm = shutil.which("llvm-nm") or shutil.which("nm")
    if not nm:
        sys.exit("llvm-nm not found on PATH")
    out = subprocess.run([nm, "--defined-only", "-n", "--no-demangle", exe], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1].lower() in ("t",):
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    # The default image base for x64 PE executables; the file records the real one.
    return addrs, names, 0x140000000


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("profile")
    ap.add_argument("exe")
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()

    lines = open(args.profile, encoding="utf-8").read().splitlines()
    m = re.match(r"base 0x([0-9a-f]+) rounds (\d+)", lines[0])
    base, rounds = int(m.group(1), 16), int(m.group(2))
    addrs, names, link_base = load_symbols(args.exe)

    # Newer profiles end with the loaded modules: samples outside the
    # executable are then named after the DLL they landed in.
    modules = []
    for line in lines[1:]:
        if line.startswith("module "):
            _, mbase, msize, mname = line.split(" ", 3)
            modules.append((int(mbase, 16), int(mbase, 16) + int(msize, 16), mname))
    modules.sort()

    def outside(rip: int) -> str:
        i = bisect.bisect_right(modules, (rip, float("inf"), "")) - 1
        if i >= 0 and modules[i][0] <= rip < modules[i][1] and modules[i][0] != base:
            return f"(outside the executable: {modules[i][2]})"
        return "(outside the executable)"

    def symbol(rip: int) -> str:
        i = bisect.bisect_right(addrs, rip - base + link_base) - 1
        return names[i] if i >= 0 else hex(rip)

    # Samples outside the executable, by the innermost exe functions on the
    # stack: what in the game or runtime called into the driver or a wait.
    callers = collections.defaultdict(collections.Counter)
    for line in lines[1:]:
        if line.startswith("callers "):
            _, thread, chain, n = line.split(" ")
            frames = [symbol(int(r, 16)) for r in chain.split(",")]
            # SDL is linked in: name its innermost entry, then the first two
            # callers outside it.
            outer = [f for f in frames if not re.match(r"(SDL_|VULKAN_|D3D|METAL_)", f)]
            inner = [f for f in frames if re.match(r"(SDL_|VULKAN_|D3D|METAL_)", f)]
            key = " <- ".join(inner[:1] + outer[:2]) if inner else " <- ".join(outer[:3])
            callers[thread.rsplit("#", 1)[0]][key] += int(n)

    per_thread = collections.Counter()
    per_func = collections.Counter()
    per_thread_func = collections.defaultdict(collections.Counter)
    for line in lines[1:]:
        if line.startswith(("module ", "callers ")):
            continue
        thread, rip, n = line.rsplit(" ", 2)
        rip, n = int(rip, 16), int(n)
        va = rip - base + link_base
        i = bisect.bisect_right(addrs, va) - 1
        name = names[i] if i >= 0 and 0 <= rip - base < 0x40000000 else outside(rip)
        thread = thread.rsplit("#", 1)[0]
        per_thread[thread] += n
        per_func[name] += n
        per_thread_func[thread][name] += n

    print(f"{rounds} sampling rounds")
    for thread, n in per_thread.most_common():
        busy = sum(c for f, c in per_thread_func[thread].items() if not f.startswith("(outside the executable"))
        print(f"\n== {thread}: {n} samples, {busy * 100 // max(n, 1)}% inside the executable")
        for name, c in per_thread_func[thread].most_common(args.top):
            print(f"  {c * 100 / n:5.1f}%  {name}")
        if callers[thread]:
            print("  -- outside the executable, by caller:")
            for chain, c in callers[thread].most_common(args.top):
                print(f"  {c * 100 / n:5.1f}%  {chain}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
