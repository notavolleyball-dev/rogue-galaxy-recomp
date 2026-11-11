"""One game function, everything static we know about it: the starting point
of a note in docs/NATIVE-ONLY-NOTES.md.

usage: fninfo.py NAME_OR_ADDRESS [--dis] [--no-source] [--rgdecomp DIR]

Prints the function's map entry (image, address, size), its callers and
callees in the main executable (jal/j sites), RGDecomp's C for it, and with
--dis its disassembly. NAME_OR_ADDRESS is a hex address (0x14f760) or a
name or part of one (ModelPrepareDraw); several matches are listed.

Needs disc/SCUS_974.90 (the extracted game) and RGDecomp at the commit
config/SCUS_974.90/funcmap.toml pins, in build/rgdecomp by default:

    git -C build/rgdecomp init && git -C build/rgdecomp remote add origin https://github.com/Dorgengoa/RGDecomp
    git -C build/rgdecomp fetch --depth 1 origin <commit> && git -C build/rgdecomp checkout FETCH_HEAD

Callers and callees cover the main executable only: overlay code
(functions_<overlay>.csv) is listed and its C printed, without call sites.
"""
import argparse
import csv
import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONFIG = ROOT / "config" / "SCUS_974.90"


def load_mipsdis():
    spec = importlib.util.spec_from_file_location("mipsdis", ROOT / "tools" / "mipsdis.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_maps():
    funcs = []
    for path in sorted(CONFIG.glob("functions_*.csv")):
        image = path.stem.removeprefix("functions_")
        with open(path, newline="") as f:
            rows = csv.reader(f)
            next(rows)
            for name, start, end, size in rows:
                funcs.append((image, name, int(start, 16), int(end, 16)))
    return funcs


def lookup(funcs, query):
    try:
        address = int(query, 16)
    except ValueError:
        address = None
    if address is not None:
        return [f for f in funcs if f[2] <= address < f[3]]
    exact = [f for f in funcs if f[1] == query]
    return exact or [f for f in funcs if query.lower() in f[1].lower()]


def jal_sites(md, data, syms, text_end):
    """(site, target) for every jal and j in the main executable."""
    out = []
    for a in range(md.BASE, text_end, 4):
        w = md.word(data, a)
        if w is not None and (w >> 26) in (2, 3):
            out.append((a, ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2), (w >> 26) == 3))
    return out


def source_for(rgdecomp, name):
    if not rgdecomp.is_dir():
        return None
    for ext in ("c", "cpp"):
        hits = list((rgdecomp / "src").rglob(f"{name}.{ext}"))
        if hits:
            return hits[0]
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("query")
    parser.add_argument("--dis", action="store_true", help="print the disassembly (main executable)")
    parser.add_argument("--no-source", action="store_true", help="do not print RGDecomp's C")
    parser.add_argument("--rgdecomp", type=Path, default=ROOT / "build" / "rgdecomp")
    args = parser.parse_args()

    funcs = load_maps()
    found = lookup(funcs, args.query)
    if not found:
        sys.exit(f"no function matches {args.query}")
    if len(found) > 1:
        for image, name, start, end in found[:60]:
            print(f"{image:8} {start:08x} {end - start:6} {name}")
        if len(found) > 60:
            print(f"... {len(found) - 60} more")
        return
    image, name, start, end = found[0]
    print(f"{name}  ({image}, {start:#010x}-{end:#010x}, {end - start} bytes)")

    elf = ROOT / "disc" / "SCUS_974.90"
    if image == "main" and elf.is_file():
        md = load_mipsdis()
        data, syms = md.load(str(elf), str(CONFIG / "functions_main.csv"))
        md.GP = md.find_gp(data) or 0x485170
        sites = jal_sites(md, data, syms, syms[-1][1])
        callers = [(a, link) for a, t, link in sites if t == start]
        print(f"\ncallers ({len(callers)} sites):")
        for a, link in callers:
            print(f"  {a:08x}  {md.symat(syms, a)}{'' if link else '  (j: a tail call)'}")
        callees = {}
        for a, t, link in sites:
            if start <= a < end:
                callees.setdefault(t, []).append(a)
        print(f"\ncallees ({len(callees)}):")
        for t, at in sorted(callees.items(), key=lambda kv: kv[1][0]):
            print(f"  {t:08x}  {md.symat(syms, t)}  (from {', '.join(f'{a:x}' for a in at)})")
        if args.dis:
            print()
            md.disrange(data, syms, start, end)
    elif image == "main":
        print(f"\n(no {elf.relative_to(ROOT)}: callers and callees need the extracted game)")

    if not args.no_source:
        path = source_for(args.rgdecomp, name)
        if path is None:
            print(f"\n(no RGDecomp source for {name} under {args.rgdecomp})")
        else:
            print(f"\n// RGDecomp: {path.relative_to(args.rgdecomp)}")
            print(path.read_text(errors="replace"))


if __name__ == "__main__":
    main()
