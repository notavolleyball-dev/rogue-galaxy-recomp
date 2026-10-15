"""Metrowerks "MWo3" overlay headers, cross-checked against the main ELF.

Each BIN/*.BIN on the disc is [0x40 header][.text][.data], loaded verbatim at
`load_addr` (header included), with .bss zero-filled after .data. The main ELF
reserves one zero-filesz PT_LOAD per overlay id, in id order, whose memsz is
exactly 0x40 + text + data + bss.

    python tools/mwo3/mwo3.py --elf disc/SCUS_974.90 disc/BIN/*.BIN
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

HEADER_SIZE = 0x40
MAGIC = b"MWo3"


@dataclass(frozen=True)
class Header:
    id: int
    load_addr: int
    text_size: int
    data_size: int
    bss_size: int
    ctor_begin: int
    ctor_end: int
    name: str

    @property
    def text_start(self) -> int:
        return self.load_addr + HEADER_SIZE

    @property
    def file_size(self) -> int:
        return HEADER_SIZE + self.text_size + self.data_size

    @property
    def mem_size(self) -> int:
        return self.file_size + self.bss_size

    @property
    def mem_end(self) -> int:
        return self.load_addr + self.mem_size


def parse_header(data: bytes) -> Header:
    if len(data) < HEADER_SIZE or data[:4] != MAGIC:
        raise ValueError("not an MWo3 overlay")
    ovl_id, load, text, dat, bss, cb, ce = struct.unpack_from("<7I", data, 4)
    name = data[0x20:HEADER_SIZE].split(b"\0")[0].decode("ascii", "replace")
    return Header(ovl_id, load, text, dat, bss, cb, ce, name)


def elf_overlay_reservations(elf: bytes) -> list[tuple[int, int]]:
    """(vaddr, memsz) of every zero-filesz, non-empty PT_LOAD, in header order."""
    if elf[:4] != b"\x7fELF":
        raise ValueError("not an ELF file")
    phoff = struct.unpack_from("<I", elf, 0x1C)[0]
    phentsize, phnum = struct.unpack_from("<HH", elf, 0x2A)
    out = []
    for i in range(phnum):
        p_type, _off, vaddr, _paddr, filesz, memsz = struct.unpack_from("<6I", elf, phoff + i * phentsize)
        if p_type == 1 and filesz == 0 and memsz > 0:
            out.append((vaddr, memsz))
    return out


def check(elf: bytes, overlays: dict[str, bytes]) -> tuple[list[tuple[str, Header]], list[str]]:
    """Parse every overlay and return (sorted headers, list of problems)."""
    problems = []
    reservations = elf_overlay_reservations(elf)
    headers = []
    for fname, data in overlays.items():
        try:
            h = parse_header(data)
        except ValueError as exc:
            problems.append(f"{fname}: {exc}")
            continue
        headers.append((fname, h))
        if len(data) != h.file_size:
            problems.append(f"{fname}: file is {len(data):#x} bytes, header says {h.file_size:#x}")
        if not 1 <= h.id <= len(reservations):
            problems.append(f"{fname}: id {h.id} has no PT_LOAD reservation")
            continue
        vaddr, memsz = reservations[h.id - 1]
        if (vaddr, memsz) != (h.load_addr, h.mem_size):
            problems.append(
                f"{fname}: ELF reserves {memsz:#x} at {vaddr:#010x}, header needs {h.mem_size:#x} at {h.load_addr:#010x}"
            )
        if h.mem_size > HEADER_SIZE and not (h.text_start <= h.ctor_begin <= h.ctor_end <= h.mem_end):
            problems.append(f"{fname}: ctor table {h.ctor_begin:#010x}..{h.ctor_end:#010x} outside the overlay")
    headers.sort(key=lambda fh: fh[1].id)
    return headers, problems


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--elf", required=True, type=Path)
    ap.add_argument("overlays", nargs="+", type=Path)
    args = ap.parse_args(argv)
    headers, problems = check(args.elf.read_bytes(), {p.name: p.read_bytes() for p in args.overlays})
    print(f"{'file':<14}{'id':>3} {'load':>10} {'text':>8} {'data':>7} {'bss':>7} {'memsz':>8} {'ctors':>23}")
    for fname, h in headers:
        print(
            f"{fname:<14}{h.id:>3} {h.load_addr:#010x} {h.text_size:#8x} {h.data_size:#7x} {h.bss_size:#7x}"
            f" {h.mem_size:#8x} {h.ctor_begin:#010x}..{h.ctor_end:#010x}"
        )
    for p in problems:
        print("PROBLEM:", p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
