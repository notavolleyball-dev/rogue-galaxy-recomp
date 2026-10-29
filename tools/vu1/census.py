#!/usr/bin/env python3
"""Every VU1 microcode piece in the game's executable, and which are native.

  tools/vu1/census.py [--elf disc/SCUS_974.90] [--recording FILE.vu1r ...]

The game uploads its VU1 code as pieces, each a DMA packet in the ELF's data:
a tag (`ret` or `cnt`) whose upper half holds the VIF codes NOP and MPG (load
address, pairs), then the code. This finds every such MPG, names its slot by
the load address, and hashes the code the way src/vu1_native.cpp does
(FNV-1a over the piece's bytes), so a known piece shows its name. With
recordings (RG_VU1_REPLAY, capture.sh or RG_VU1_HARVEST), it also says
which pieces the recorded programs held, and how many runs began with each.

No game code is printed: addresses, sizes and hashes only.
"""
import argparse
import re
import struct
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# By load address (pairs): the slots of docs/VU1-NATIVE.md. Pieces of C and D
# load at a few other addresses inside those slots.
SLOTS = [(0x000, "base"), (0x064, "A"), (0x12C, "B"), (0x21C, "C"), (0x320, "D"), (0x4C4, "E"), (0x73A, "F")]


def slot_of(address):
    name = "?"
    for start, slot in SLOTS:
        if address >= start:
            name = slot
    return name


def fnv(data):
    h = 14695981039346656037
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def known_pieces():
    """(hash -> name) from the piece tables of src/vu1_native.cpp."""
    text = (ROOT / "src" / "vu1_native.cpp").read_text()
    names = {}
    common = re.search(r"kCommon\[\] = \{(.*?)\};", text, re.S).group(1)
    for slot, h in zip("ABCF", re.findall(r"0x([0-9a-f]{16})ull", common)):
        names[int(h, 16)] = slot
    for h, name in re.findall(r"0x([0-9a-f]{16})ull\}, \{\"([^\"]+)\"", text):
        names[int(h, 16)] = "D: " + name
    return names


def elf_segments(elf):
    """(file offset, size, virtual address) of each loadable segment."""
    phoff, = struct.unpack_from("<I", elf, 0x1C)
    phentsize, phnum = struct.unpack_from("<HH", elf, 0x2A)
    out = []
    for i in range(phnum):
        ptype, offset, vaddr, _, filesz = struct.unpack_from("<5I", elf, phoff + i * phentsize)
        if ptype == 1:
            out.append((offset, filesz, vaddr))
    return out


def find_pieces(elf):
    """Every upload: a DMA tag (cnt or ret) whose VIF codes are NOPs and MPGs,
    the MPGs of one packet that continue each other joined into one piece
    (an MPG carries at most 256 pairs)."""
    pieces = []
    for offset, size, vaddr in elf_segments(elf):
        for q in range(offset - offset % 16, offset + size - 16, 16):
            tag, = struct.unpack_from("<I", elf, q)
            qwc, tag_id = tag & 0xFFFF, (tag >> 28) & 7
            if tag_id not in (1, 6) or qwc == 0:  # cnt, ret
                continue
            vif1, = struct.unpack_from("<I", elf, q + 12)
            if (vif1 >> 24) & 0x7F != 0x4A:
                continue
            # The VIF stream: the tag's upper half, then its qwc quadwords.
            pos, stream_end = q + 8, q + 16 + qwc * 16
            piece = None
            while pos + 4 <= stream_end:
                code, = struct.unpack_from("<I", elf, pos)
                command = (code >> 24) & 0x7F
                pos += 4
                if command == 0x00:
                    continue
                if command != 0x4A or pos % 8:
                    break
                pairs = (code >> 16) & 0xFF or 256
                load = code & 0x7FF
                if load + pairs > 2048 or pos + pairs * 8 > stream_end:
                    break
                data = elf[pos:pos + pairs * 8]
                if piece and piece["load"] + piece["pairs"] == load:
                    piece["pairs"] += pairs
                    piece["code"] += data
                else:
                    if piece:
                        pieces.append(piece)
                    piece = {"offset": pos, "address": vaddr + (pos - offset), "load": load, "pairs": pairs,
                             "code": data}
                pos += pairs * 8
            if piece:
                pieces.append(piece)
    for p in pieces:
        p["hash"] = fnv(p["code"])
    return pieces


def recorded_images(paths):
    """(image hash -> code) and (image hash -> MSCAL runs) of recordings."""
    images, runs = {}, defaultdict(int)
    for path in paths:
        with open(path, "rb") as f:
            state_size = struct.unpack("<5I", f.read(20))[2]
            while True:
                kind = f.read(4)
                if len(kind) < 4:
                    break
                kind, = struct.unpack("<I", kind)
                if kind == 0x45444F43:
                    h, = struct.unpack("<Q", f.read(8))
                    images[h] = f.read(16384)
                elif kind == 0x204E5552:
                    header = f.read(40)
                    flags, start_pc = struct.unpack_from("<II", header, 0)
                    code_hash, = struct.unpack_from("<Q", header, 32)
                    if not flags & 1:
                        runs[code_hash] += 1
                    f.seek(state_size + 16384, 1)
                elif kind == 0x4B43494B:
                    f.seek(struct.unpack("<I", f.read(4))[0], 1)
                elif kind == 0x20444E45:
                    flags, = struct.unpack("<I", f.read(4))
                    f.seek(state_size + (16384 if flags & 1 else 0), 1)
                else:
                    break
    return images, runs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=str(ROOT / "disc" / "SCUS_974.90"))
    ap.add_argument("--recording", nargs="*", default=[])
    a = ap.parse_args()
    elf = open(a.elf, "rb").read()
    pieces = find_pieces(elf)
    names = known_pieces()
    images, runs = recorded_images(a.recording)

    # A piece is in a recorded image when its code sits at its load address.
    seen = defaultdict(int)
    for h, code in images.items():
        for p in pieces:
            at = p["load"] * 8
            if code[at:at + p["pairs"] * 8] == p["code"]:
                seen[p["offset"]] += runs[h]

    distinct = {}
    for p in pieces:
        distinct.setdefault((p["load"], p["hash"]), p)
    print("%d MPG uploads in %s, %d distinct pieces (by load address and code)\n" %
          (len(pieces), Path(a.elf).name, len(distinct)))
    print("%-4s %-6s %6s %10s %-17s %8s  %s" % ("slot", "load", "pairs", "address", "hash", "runs" if a.recording else "", "native"))
    done = 0
    for (load, h), p in sorted(distinct.items()):
        name = names.get(h, "")
        done += 1 if name else 0
        count = sum(seen[q["offset"]] for q in pieces if (q["load"], q["hash"]) == (load, h))
        print("%-4s 0x%03x %6d 0x%08x %016x %8s  %s" % (slot_of(load), load, p["pairs"], p["address"], h,
                                                       (count if count else "-") if a.recording else "", name))
    print("\nnative: %d of %d pieces" % (done, len(distinct)))
    if a.recording:
        unseen = sum(1 for k, p in distinct.items()
                     if not any(seen[q["offset"]] for q in pieces if (q["load"], q["hash"]) == k))
        print("never in these recordings: %d pieces (need a scene that runs them, or RG_VU1_HARVEST)" % unseen)


if __name__ == "__main__":
    main()
