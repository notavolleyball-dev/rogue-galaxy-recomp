"""Compare two capture sets of tools/run/reference.sh (docs/NATIVE-ONLY.md).

usage: framediff.py A B [--tolerance N] [--max-pixels N] [--heatmaps DIR] [--quiet]
       framediff.py --root build/reference LABEL_A LABEL_B [...]

A and B are capture directories (build/reference/ROUTE/LABEL) holding
frame_NNNNNN.ppm files. With --root, A and B are labels, and every route
under the root that has both is compared.

Per frame: the pixels whose largest channel difference is over the
tolerance, the largest difference, and the bounding box of those pixels.
A frame passes when no more than --max-pixels pixels are over the tolerance.
With --heatmaps, each failing frame gets a PNG: the frame from B dimmed,
with the pixels over the tolerance in red (brighter for larger differences).
Frames in one set and not the other are listed. Exit status 0 when every
frame passes and both sets hold the same frames, 1 otherwise.

    framediff.py build/reference/walk/tag-native build/reference/walk/m1
    framediff.py --root build/reference tag-native m2 --tolerance 2 --heatmaps build/diff
"""
import argparse
import re
import struct
import sys
import zlib
from pathlib import Path

FRAME = re.compile(r"frame_(\d+)\.ppm$")


def read_ppm(path):
    data = path.read_bytes()
    # P6, then width, height and maxval separated by whitespace (and
    # comments), then one whitespace byte and the pixels.
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    if fields[0] != b"P6" or fields[3] != b"255":
        raise ValueError(f"{path}: not an 8-bit P6 PPM")
    width, height = int(fields[1]), int(fields[2])
    pixels = data[pos + 1:pos + 1 + width * height * 3]
    if len(pixels) != width * height * 3:
        raise ValueError(f"{path}: truncated")
    return width, height, pixels


def write_png(path, width, height, rgb):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    rows = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


def compare(a, b, tolerance):
    """Pixels over the tolerance, the largest difference, their bounding box
    and the per-pixel differences (None when the sizes differ)."""
    wa, ha, pa = a
    wb, hb, pb = b
    if (wa, ha) != (wb, hb):
        return None
    n = wa * ha
    # The largest channel difference per pixel (plain Python: about a
    # second a 640x448 frame, which is fine for tens of frames a route).
    da = [abs(x - y) for x, y in zip(pa, pb)]
    diff = [max(da[i], da[i + 1], da[i + 2]) for i in range(0, n * 3, 3)]
    over = [i for i, d in enumerate(diff) if d > tolerance]
    largest = max(diff) if diff else 0
    box = None
    if over:
        xs = [i % wa for i in over]
        ys = [i // wa for i in over]
        box = (min(xs), min(ys), max(xs), max(ys))
    return len(over), largest, box, diff


def heatmap(b, diff, tolerance):
    w, h, pb = b
    out = bytearray(w * h * 3)
    for i, d in enumerate(diff):
        j = i * 3
        if d > tolerance:
            out[j] = 128 + min(127, d)
            out[j + 1] = 0
            out[j + 2] = 0
        else:
            grey = (pb[j] + pb[j + 1] + pb[j + 2]) // 12
            out[j] = out[j + 1] = out[j + 2] = grey
    return bytes(out)


def frames(directory):
    return {int(m.group(1)): p for p in directory.iterdir() if (m := FRAME.search(p.name))}


def compare_sets(a_dir, b_dir, args, label):
    fa, fb = frames(a_dir), frames(b_dir)
    ok = True
    only_a, only_b = sorted(fa.keys() - fb.keys()), sorted(fb.keys() - fa.keys())
    if only_a or only_b:
        ok = False
        if only_a:
            print(f"{label}: only in A: {', '.join(map(str, only_a[:12]))}{' ...' if len(only_a) > 12 else ''}")
        if only_b:
            print(f"{label}: only in B: {', '.join(map(str, only_b[:12]))}{' ...' if len(only_b) > 12 else ''}")
    failed = 0
    common = sorted(fa.keys() & fb.keys())
    for n in common:
        a, b = read_ppm(fa[n]), read_ppm(fb[n])
        result = compare(a, b, args.tolerance)
        if result is None:
            print(f"{label} frame {n}: sizes differ ({a[0]}x{a[1]} against {b[0]}x{b[1]})")
            failed += 1
            continue
        count, largest, box, diff = result
        bad = count > args.max_pixels
        if bad:
            failed += 1
        if bad or not args.quiet:
            where = f", in {box[0]},{box[1]}-{box[2]},{box[3]}" if box else ""
            print(f"{label} frame {n}: {'FAIL' if bad else 'ok'}  {count} pixels over {args.tolerance}, "
                  f"largest {largest}{where}")
        if bad and args.heatmaps:
            out = Path(args.heatmaps) / label
            out.mkdir(parents=True, exist_ok=True)
            write_png(out / f"frame_{n:06d}.png", b[0], b[1], heatmap(b, diff, args.tolerance))
    print(f"{label}: {len(common) - failed} of {len(common)} frames pass")
    return ok and failed == 0 and len(common) > 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("a", type=Path)
    parser.add_argument("b", type=Path)
    parser.add_argument("--tolerance", type=int, default=0, help="largest channel difference allowed (default 0)")
    parser.add_argument("--max-pixels", type=int, default=0, help="pixels over the tolerance a frame may have (default 0)")
    parser.add_argument("--heatmaps", help="write a PNG per failing frame here")
    parser.add_argument("--quiet", action="store_true", help="print failing frames and summaries only")
    parser.add_argument("--root", type=Path, help="A and B are labels under ROOT/<route>/")
    args = parser.parse_args()
    if args.root:
        routes = sorted(r.name for r in args.root.iterdir()
                        if r.is_dir() and (r / str(args.a)).is_dir() and (r / str(args.b)).is_dir())
        if not routes:
            sys.exit(f"no route under {args.root} has both {args.a} and {args.b}")
        ok = all([compare_sets(args.root / r / str(args.a), args.root / r / str(args.b), args, r) for r in routes])
    else:
        for d in (args.a, args.b):
            if not d.is_dir():
                sys.exit(f"{d}: not a directory")
        ok = compare_sets(args.a, args.b, args, args.a.parent.name or "frames")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
