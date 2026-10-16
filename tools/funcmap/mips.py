"""Just enough R5900 decoding to follow control flow in the function maps."""

from __future__ import annotations

JR_RA = 0x03E00008


def jump_target(addr: int, word: int) -> int | None:
    """Target of J/JAL at `addr`, else None."""
    if word >> 26 in (2, 3):
        return ((addr + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
    return None


def is_jal(word: int) -> bool:
    return word >> 26 == 3


def branch_target(addr: int, word: int) -> int | None:
    """Target of a PC-relative branch (incl. likely, REGIMM and BCx), else None."""
    op = word >> 26
    rs = (word >> 21) & 31
    rt = (word >> 16) & 31
    off = word & 0xFFFF
    if off & 0x8000:
        off -= 0x10000
    if (
        op in (4, 5, 6, 7, 20, 21, 22, 23)  # beq bne blez bgtz + likely forms
        or (op == 1 and rt in (0, 1, 2, 3, 16, 17, 18, 19))  # bltz bgez bltzal bgezal (+l)
        or (op in (16, 17, 18) and rs == 8)  # bc0/bc1/bc2
    ):
        return (addr + 4 + off * 4) & 0xFFFFFFFF
    return None


def is_unconditional(word: int) -> bool:
    """J, JR or `beq zero, zero` (b): control never falls through."""
    op = word >> 26
    if op == 2:
        return True
    if op == 0 and word & 63 == 8:
        return True
    return op == 4 and (word >> 16) & 0x3FF == 0
