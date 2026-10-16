"""Guest images as they sit in EE memory: the main ELF and the MWo3 overlays."""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

from mwo3 import mwo3


@dataclass
class Image:
    name: str
    base: int  # guest address of data[0]
    data: bytes
    code: list[tuple[int, int]]  # ranges the map may cover
    header: mwo3.Header | None = None  # overlays only

    @property
    def end(self) -> int:
        return self.base + len(self.data)

    def word(self, addr: int) -> int:
        return struct.unpack_from("<I", self.data, addr - self.base)[0]

    def in_code(self, addr: int) -> bool:
        return any(lo <= addr < hi for lo, hi in self.code)


def load_main(path: Path, code: list[tuple[int, int]]) -> Image:
    """The file image of the first PT_LOAD (the only one with bytes)."""
    elf = path.read_bytes()
    if elf[:4] != b"\x7fELF":
        raise ValueError(f"{path}: not an ELF file")
    phoff = struct.unpack_from("<I", elf, 0x1C)[0]
    phentsize, phnum = struct.unpack_from("<HH", elf, 0x2A)
    for i in range(phnum):
        p_type, off, vaddr, _paddr, filesz, _memsz = struct.unpack_from("<6I", elf, phoff + i * phentsize)
        if p_type == 1 and filesz:
            return Image("main", vaddr, elf[off : off + filesz], code)
    raise ValueError(f"{path}: no loadable bytes")


def load_overlay(name: str, path: Path) -> Image:
    """An overlay as loaded: header at load_addr, then .text and .data.

    The map may cover .text and .data: every overlay keeps its static
    constructors in .data, right before the ctor table.
    """
    data = path.read_bytes()
    h = mwo3.parse_header(data)
    return Image(name, h.load_addr, data, [(h.text_start, h.load_addr + h.file_size)], h)
