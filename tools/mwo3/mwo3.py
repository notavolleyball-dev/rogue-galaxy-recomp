"""Metrowerks "MWo3" overlay headers, cross-checked against the main ELF.

Each BIN/*.BIN on the disc is [0x40 header][.text][.data], loaded verbatim at
`load_addr` (header included), with .bss zero-filled after .data. The main ELF
reserves one zero-filesz PT_LOAD per overlay id, in id order, whose memsz is
exactly 0x40 + text + data + bss.

    python tools/mwo3/mwo3.py --elf disc/SCUS_974.90 disc/BIN/*.BIN
    python tools/mwo3/mwo3.py --elf disc/SCUS_974.90 --to-elf build/overlays disc/BIN/TITLE.BIN
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


# ELF32 little-endian constants for to_elf().
_PT_LOAD = 1
_PF_X, _PF_W, _PF_R = 1, 2, 4
_SHT_PROGBITS, _SHT_NOBITS, _SHT_STRTAB = 1, 8, 3
_SHF_WRITE, _SHF_ALLOC, _SHF_EXECINSTR = 1, 2, 4
_EM_MIPS = 8
_EF_R5900 = 0x20924000  # e_flags of SCUS_974.90 itself


def to_elf(data: bytes) -> bytes:
    """Wrap an overlay as an executable ELF that ps2_recomp can read.

    .text sits at load_addr + 0x40, as in RAM. .data is marked executable
    too: every overlay's static constructors live there, and ps2_recomp only
    accepts map functions inside executable sections (the main ELF is one
    RWX section for the same reason). .bss is a NOBITS section. The 0x40-byte
    header itself is left out; no code refers to it.
    """
    h = parse_header(data)
    if len(data) != h.file_size:
        raise ValueError(f"file is {len(data):#x} bytes, header says {h.file_size:#x}")
    text = data[HEADER_SIZE : HEADER_SIZE + h.text_size]
    dat = data[HEADER_SIZE + h.text_size :]
    text_addr = h.text_start
    data_addr = text_addr + h.text_size
    bss_addr = data_addr + h.data_size

    names = ["", ".text", ".data"] + ([".bss"] if h.bss_size else []) + [".shstrtab"]
    shstrtab = b"\0".join(n.encode() for n in names) + b"\0"
    name_off = {n: shstrtab.index(n.encode() + b"\0") if n else 0 for n in names}

    segs = [(text_addr, text, h.text_size, _PF_R | _PF_X), (data_addr, dat, h.data_size, _PF_R | _PF_W | _PF_X)]
    if h.bss_size:
        segs.append((bss_addr, b"", h.bss_size, _PF_R | _PF_W))
    phoff = 0x34
    body = phoff + 0x20 * len(segs)
    text_off = (body + 15) & ~15
    data_off = text_off + len(text)
    str_off = data_off + len(dat)
    shoff = (str_off + len(shstrtab) + 3) & ~3

    out = bytearray(b"\x7fELF\x01\x01\x01" + bytes(9))
    out += struct.pack("<HHIIIIIHHHHHH", 2, _EM_MIPS, 1, text_addr, phoff, shoff, _EF_R5900, 0x34, 0x20,
                       len(segs), 0x28, len(names), len(names) - 1)
    for (vaddr, blob, memsz, flags), off in zip(segs, (text_off, data_off, str_off)):
        out += struct.pack("<8I", _PT_LOAD, off, vaddr, vaddr, len(blob), memsz, flags, 16)
    out += bytes(text_off - len(out)) + text + dat + shstrtab
    out += bytes(shoff - len(out))

    def sh(name, sh_type, flags, addr, off, size, align):
        return struct.pack("<10I", name_off[name], sh_type, flags, addr, off, size, 0, 0, align, 0)

    out += bytes(0x28)
    out += sh(".text", _SHT_PROGBITS, _SHF_ALLOC | _SHF_EXECINSTR, text_addr, text_off, len(text), 16)
    out += sh(".data", _SHT_PROGBITS, _SHF_ALLOC | _SHF_WRITE | _SHF_EXECINSTR, data_addr, data_off, len(dat), 16)
    if h.bss_size:
        out += sh(".bss", _SHT_NOBITS, _SHF_ALLOC | _SHF_WRITE, bss_addr, str_off, h.bss_size, 16)
    out += sh(".shstrtab", _SHT_STRTAB, 0, 0, str_off, len(shstrtab), 1)
    return bytes(out)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--elf", required=True, type=Path)
    ap.add_argument("--to-elf", type=Path, metavar="DIR", help="also write <name>.elf for each overlay into DIR")
    ap.add_argument("overlays", nargs="+", type=Path)
    args = ap.parse_args(argv)
    if args.to_elf:
        args.to_elf.mkdir(parents=True, exist_ok=True)
        for p in args.overlays:
            (args.to_elf / (p.stem.lower() + ".elf")).write_bytes(to_elf(p.read_bytes()))
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
