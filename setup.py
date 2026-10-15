#!/usr/bin/env python3
"""Rogue Galaxy Recompiled: disc tooling.

    python setup.py verify  --iso "Rogue Galaxy (USA).iso" [--full]
    python setup.py extract --iso "Rogue Galaxy (USA).iso" [--out disc] [--force]
    python setup.py info    [--disc disc]

`verify` checks a dump against config/<version>/hashes.json without writing
anything. `extract` unpacks both DVD layers into one tree (the runtime boots
the ELF from that folder) and hashes every file while copying. `info` decodes
the MWo3 overlay table from an extracted tree. `make-hashes` (maintainers)
regenerates hashes.json from a known-good dump.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import zlib
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))

from disc.iso9660 import SECTOR, Entry, IsoImage  # noqa: E402
from mwo3 import mwo3  # noqa: E402

DEFAULT_VERSION = "SCUS_974.90"
QUICK_LIMIT = 32 << 20  # files up to this size are hashed by plain `verify`


def config_dir(version: str) -> Path:
    return ROOT / "config" / version


def load_hashes(version: str) -> dict:
    path = config_dir(version) / "hashes.json"
    if not path.is_file():
        sys.exit(f"no known hashes for {version} ({path})")
    return json.loads(path.read_text(encoding="utf-8"))


def unique_files(iso: IsoImage) -> dict[str, list[Entry]]:
    """Every file path on the disc, mapped to its copies (layer 0 first)."""
    files: dict[str, list[Entry]] = {}
    for e in iso.entries():
        if not e.is_dir:
            files.setdefault(e.path, []).append(e)
    return files


def hash_entry(iso: IsoImage, entry: Entry) -> str:
    h = hashlib.sha256()
    for chunk in iso.iter_chunks(entry):
        h.update(chunk)
    return h.hexdigest()


def image_digests(path: Path) -> dict:
    md5, sha1, sha256, crc = hashlib.md5(), hashlib.sha1(), hashlib.sha256(), 0
    with open(path, "rb") as f:
        while chunk := f.read(8 << 20):
            md5.update(chunk)
            sha1.update(chunk)
            sha256.update(chunk)
            crc = zlib.crc32(chunk, crc)
    return {"crc32": f"{crc:08x}", "md5": md5.hexdigest(), "sha1": sha1.hexdigest(), "sha256": sha256.hexdigest()}


def check_layout(iso: IsoImage, known: dict) -> list[str]:
    problems = []
    k = known["iso"]
    if iso.size != k["size"]:
        problems.append(f"image is {iso.size} bytes, expected {k['size']}")
    starts = [v.base for v in iso.volumes]
    if starts != k["volume_starts"]:
        problems.append(f"ISO9660 volumes start at sectors {starts}, expected {k['volume_starts']}")
    return problems


def cmd_verify(args: argparse.Namespace) -> int:
    known = load_hashes(args.version)
    with IsoImage(args.iso) as iso:
        problems = check_layout(iso, known)
        files = unique_files(iso)
        expected = known["files"]
        for path in sorted(set(expected) - set(files)):
            problems.append(f"missing from image: {path}")
        for path in sorted(set(files) - set(expected)):
            problems.append(f"not in hashes.json: {path}")
        checked = 0
        for path, info in sorted(expected.items()):
            copies = files.get(path)
            if not copies:
                continue
            layers = [c.layer for c in copies]
            if layers != info["layers"]:
                problems.append(f"{path}: on layers {layers}, expected {info['layers']}")
            if copies[0].size != info["size"]:
                problems.append(f"{path}: {copies[0].size} bytes, expected {info['size']}")
                continue
            if info["size"] <= QUICK_LIMIT:
                if hash_entry(iso, copies[0]) != info["sha256"]:
                    problems.append(f"{path}: sha256 mismatch")
                checked += 1
    print(f"layout and sizes checked for {len(expected)} files; {checked} small files hashed")
    if args.full:
        print("hashing the whole image (about a minute)...")
        got = image_digests(Path(args.iso))
        for algo in ("crc32", "md5", "sha1"):
            if got[algo] != known["iso"][algo]:
                problems.append(f"image {algo} {got[algo]} != Redump {known['iso'][algo]}")
        if not problems:
            print(f"image matches Redump: crc32 {got['crc32']}, sha1 {got['sha1']}")
    for p in problems:
        print("PROBLEM:", p)
    print("OK" if not problems else f"{len(problems)} problem(s)")
    return 1 if problems else 0


def cmd_extract(args: argparse.Namespace) -> int:
    known = load_hashes(args.version)
    out = Path(args.out)
    if out.exists() and any(out.iterdir()) and not args.force:
        sys.exit(f"{out} is not empty; pass --force to overwrite")
    problems = []
    manifest = {"version": args.version, "image": str(Path(args.iso).resolve()), "files": {}}
    with IsoImage(args.iso) as iso:
        problems += check_layout(iso, known)
        files = unique_files(iso)
        total = sum(c[0].size for c in files.values())
        done = 0
        for path, copies in sorted(files.items(), key=lambda kv: kv[1][0].lba):
            first = copies[0]
            dest = out / path
            dest.parent.mkdir(parents=True, exist_ok=True)
            part = dest.with_name(dest.name + ".part")
            h = hashlib.sha256()
            with open(part, "wb") as f:
                iso.copy_to(first, f, h.update)
            part.replace(dest)
            digest = h.hexdigest()
            info = known["files"].get(path)
            if info is None:
                problems.append(f"not in hashes.json: {path}")
            elif digest != info["sha256"]:
                problems.append(f"{path}: sha256 mismatch")
            manifest["files"][path] = {
                "size": first.size,
                "sha256": digest,
                "copies": [{"layer": c.layer, "lba": c.lba, "layer_lba": c.layer_lba} for c in copies],
            }
            done += first.size
            print(f"\r{done * 100 // max(total, 1):3d}%  {path:<40.40}", end="", flush=True)
        print()
        manifest["volumes"] = [
            {"layer": v.layer, "base": v.base, "blocks": v.blocks, "volume_id": v.volume_id} for v in iso.volumes
        ]
    for path in sorted(set(known["files"]) - set(manifest["files"])):
        problems.append(f"missing from image: {path}")
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"extracted {len(manifest['files'])} files ({total / 2**30:.2f} GiB) to {out}")
    _, ovl_problems = overlay_table(out, known["elf"], quiet=True)
    problems += ovl_problems
    for p in problems:
        print("PROBLEM:", p)
    print("OK" if not problems else f"{len(problems)} problem(s)")
    return 1 if problems else 0


def overlay_table(disc: Path, elf_name: str, quiet: bool = False):
    elf = (disc / elf_name).read_bytes()
    bins = {p.name: p.read_bytes() for p in sorted((disc / "BIN").glob("*.BIN"))}
    headers, problems = mwo3.check(elf, bins)
    if not quiet:
        for fname, h in headers:
            print(
                f"  {h.id}  {fname:<13} load {h.load_addr:#010x}  text {h.text_size:#8x}  data {h.data_size:#7x}"
                f"  bss {h.bss_size:#7x}  mem {h.mem_size:#8x}  ctors {h.ctor_begin:#010x}..{h.ctor_end:#010x}"
            )
    return headers, problems


def cmd_info(args: argparse.Namespace) -> int:
    known = load_hashes(args.version)
    disc = Path(args.disc)
    print((disc / "SYSTEM.CNF").read_text(encoding="ascii", errors="replace").strip())
    print("MWo3 overlays:")
    headers, problems = overlay_table(disc, known["elf"])
    if headers:
        arena_start = min(h.load_addr for _, h in headers)
        arena_end = max(h.mem_end for _, h in headers)
        print(f"arena {arena_start:#010x}..{arena_end:#010x} ({(arena_end - arena_start) / 1024:.1f} KiB)")
    for p in problems:
        print("PROBLEM:", p)
    return 1 if problems else 0


def cmd_make_hashes(args: argparse.Namespace) -> int:
    path = config_dir(args.version) / "hashes.json"
    old = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}
    with IsoImage(args.iso) as iso:
        files = {}
        for p, copies in sorted(unique_files(iso).items()):
            digests = {hash_entry(iso, c) for c in copies}
            if len(digests) != 1:
                sys.exit(f"{p}: copies on different layers differ; hashes.json cannot describe that yet")
            files[p] = {"size": copies[0].size, "sha256": digests.pop(), "layers": [c.layer for c in copies]}
        iso_info = {
            "size": iso.size,
            "sectors": iso.total_sectors,
            "volume_starts": [v.base for v in iso.volumes],
        }
    digests = image_digests(Path(args.iso))
    for algo in ("crc32", "md5", "sha1"):
        # these come from Redump; refuse to overwrite them from a different dump
        if algo in old.get("iso", {}) and old["iso"][algo] != digests[algo]:
            sys.exit(f"image {algo} {digests[algo]} does not match the Redump value {old['iso'][algo]} in {path}")
    iso_info.update(digests)
    data = {
        **{k: v for k, v in old.items() if k not in ("iso", "files", "generated")},
        "generated": date.today().isoformat(),
        "iso": {**old.get("iso", {}), **iso_info},
        "files": files,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {path} ({len(files)} files)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Rogue Galaxy Recompiled disc tooling")
    ap.add_argument("--version", default=DEFAULT_VERSION, help=f"config/<version> to use (default {DEFAULT_VERSION})")
    sub = ap.add_subparsers(dest="command", required=True)

    p = sub.add_parser("verify", help="check a dump against known hashes (read-only)")
    p.add_argument("--iso", required=True)
    p.add_argument("--full", action="store_true", help="also hash the whole image against Redump")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("extract", help="unpack both layers into one folder")
    p.add_argument("--iso", required=True)
    p.add_argument("--out", default=str(ROOT / "disc"))
    p.add_argument("--force", action="store_true", help="overwrite a non-empty output folder")
    p.set_defaults(func=cmd_extract)

    p = sub.add_parser("info", help="print SYSTEM.CNF and the MWo3 overlay table")
    p.add_argument("--disc", default=str(ROOT / "disc"))
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("make-hashes", help="(maintainers) regenerate hashes.json from a known-good dump")
    p.add_argument("--iso", required=True)
    p.set_defaults(func=cmd_make_hashes)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
