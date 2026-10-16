"""Checks the function maps and recompiler configs against the disc images.

Everything here reads the user's extracted disc and the pinned framework; it
never writes. `python setup.py check` runs it all.
"""

from __future__ import annotations

import bisect
import re
from collections import Counter
from dataclasses import dataclass, field

from funcmap import mips
from funcmap.funcmap import RESERVED_PREFIXES, Function
from funcmap.images import Image
from funcmap.runtime_calls import RuntimeCalls


@dataclass
class Report:
    problems: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    def merge(self, other: "Report") -> None:
        self.problems += other.problems
        self.notes += other.notes


class Index:
    """Start lookup and containment for one map."""

    def __init__(self, funcs: list[Function]):
        self.funcs = funcs
        self.starts = [f.start for f in funcs]
        self.start_set = set(self.starts)

    def containing(self, addr: int) -> Function | None:
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0 and self.funcs[i].start <= addr < self.funcs[i].end:
            return self.funcs[i]
        return None


def _sanitized(name: str) -> str:
    # ps2_recomp's sanitizeIdentifierBody: anything but [A-Za-z0-9_] becomes '_'
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


def check_names(maps: dict[str, list[Function]], calls: RuntimeCalls | None) -> Report:
    r = Report()
    seen = Counter(_sanitized(f.name) for funcs in maps.values() for f in funcs)
    for name, n in seen.items():
        if n > 1:
            r.problems.append(f"name {name} is used {n} times across the maps")
    for image, funcs in maps.items():
        for f in funcs:
            if f.name.startswith(RESERVED_PREFIXES):
                r.problems.append(f"{image}: {f.name} uses a name prefix ps2_recomp reserves")
            if calls and calls.binds_by_name(f.name):
                r.problems.append(f"{image}: {f.name} would bind to a runtime handler by name")
    return r


def check_layout(img: Image, funcs: list[Function], known_gaps: list[tuple[int, int]]) -> Report:
    """Bounds, overlaps, and code left outside every function."""
    r = Report()
    for f in funcs:
        if f.start % 4 or f.end % 4 or f.end <= f.start:
            r.problems.append(f"{img.name}: {f.name} has bad bounds {f.start:#x}..{f.end:#x}")
        elif not any(lo <= f.start and f.end <= hi for lo, hi in img.code):
            r.problems.append(f"{img.name}: {f.name} ({f.start:#x}..{f.end:#x}) is outside the code ranges")
    for a, b in zip(funcs, funcs[1:]):
        if a.end > b.start:
            r.problems.append(f"{img.name}: {a.name} overlaps {b.name}")

    # Non-zero words between functions are code the map misses (or data in
    # code, which must then be listed as a known gap). Overlay .data is data.
    ranges = img.code
    if img.header:
        ranges = [(img.header.text_start, img.header.text_start + img.header.text_size)]
    for lo, hi in ranges:
        inside = [f for f in funcs if lo <= f.start < hi]
        edges = [lo] + [x for f in inside for x in (f.start, f.end)] + [hi]
        for g0, g1 in zip(edges[::2], edges[1::2]):
            nz = [a for a in range(g0, g1, 4) if img.word(a)]
            if not nz:
                continue
            if any(k0 <= nz[0] and nz[-1] < k1 for k0, k1 in known_gaps):
                continue
            r.problems.append(f"{img.name}: {len(nz)} non-zero word(s) outside any function in {g0:#x}..{g1:#x}")
    covered = sum(f.size for f in funcs)
    r.notes.append(f"{img.name}: {len(funcs)} functions, {covered:#x} bytes, {len(known_gaps)} known gap(s)")
    return r


def check_ctors(img: Image, idx: Index, table: tuple[int, int]) -> Report:
    """Every entry of a static constructor table [begin, end) is a function start."""
    r = Report()
    begin, end = table
    for slot in range(begin, end, 4):
        target = img.word(slot)
        if target not in idx.start_set:
            r.problems.append(f"{img.name}: ctor table entry {slot:#x} -> {target:#x} is not a function start")
    if not r.problems:
        r.notes.append(f"{img.name}: {(end - begin) // 4} static constructors, all mapped")
    return r


def check_control_flow(images: dict[str, Image], maps: dict[str, list[Function]], arena: tuple[int, int]) -> Report:
    """Every J/JAL must land where some image's recompiled code can be entered.

    Inside one image, a jump into the middle of another function is fine:
    ps2_recomp synthesizes an `entry_<addr>` for it. Across images it is not,
    since each image is recompiled on its own: an overlay calling the main ELF
    must hit a main function start, and the main ELF calling into the arena
    must hit a start in at least one overlay.
    """
    r = Report()
    idx = {name: Index(funcs) for name, funcs in maps.items()}
    overlay_starts = set().union(*(i.start_set for n, i in idx.items() if n != "main"))
    for name, funcs in maps.items():
        img, own = images[name], idx[name]
        mid_entries: set[int] = set()
        to_arena = 0
        for f in funcs:
            for addr in range(f.start, f.end, 4):
                word = img.word(addr)
                target = mips.jump_target(addr, word)
                if target is None:
                    continue
                if not mips.is_jal(word) and f.start <= target < f.end:
                    continue  # plain jump inside the function
                if target in own.start_set:
                    continue
                if own.containing(target):
                    mid_entries.add(target)
                    continue
                if name == "main" and arena[0] <= target < arena[1]:
                    to_arena += 1
                    if target not in overlay_starts:
                        r.problems.append(f"main: {addr:#x} calls {target:#x} in the arena, a start in no overlay")
                    continue
                if name != "main" and target in idx["main"].start_set:
                    continue
                where = "the main ELF" if name != "main" and idx["main"].containing(target) else "no function"
                r.problems.append(f"{name}: {addr:#x} jumps to {target:#x}, inside {where}")
        r.notes.append(
            f"{name}: {len(mid_entries)} jump target(s) inside other functions (ps2_recomp adds entries)"
            + (f", {to_arena} direct call(s) into the overlay arena" if to_arena else "")
        )
    return r


def check_pointer_targets(images: dict[str, Image], maps: dict[str, list[Function]]) -> Report:
    """Code addresses taken as data that point into the middle of a function.

    A word in any image's data, or a lui/addiu (ori) pair in its code, whose
    value lands right after a `jr ra` and its delay slot inside a mapped
    function is almost always a function the map merged into its neighbour:
    callbacks reached only through pointers, often from another image (the
    main ELF's tables point into overlay code). Arbitrary mid-function values
    are too common in data to flag; this shape is not.
    """
    r = Report()
    idx = {name: Index(funcs) for name, funcs in maps.items()}
    hits: dict[tuple[str, int], str] = {}

    def follows_return(img: Image, f: Function, value: int) -> bool:
        # `jr ra`, its delay slot (possibly a nop), then zero padding up to value.
        addr = value - 4
        while addr >= f.start and value - addr <= 16 and img.word(addr) == 0:
            addr -= 4
        if addr < f.start:
            return False
        if img.word(addr) == mips.JR_RA:
            return addr + 8 <= value
        return addr - 4 >= f.start and img.word(addr - 4) == mips.JR_RA

    def consider(value: int, source: str, from_image: str) -> None:
        if value % 4:
            return
        for name, img in images.items():
            # Overlays are never resident together: one only points at itself or the main ELF.
            if from_image != "main" and name not in ("main", from_image):
                continue
            if not img.in_code(value) or value in idx[name].start_set:
                continue
            f = idx[name].containing(value)
            if f and follows_return(img, f, value):
                hits.setdefault((name, value), f"{f.name}, from {source}")

    for name, img in images.items():
        own = idx[name]
        for addr in range(img.base, img.end - 3, 4):
            if own.containing(addr) is None:
                consider(img.word(addr), f"{name} data at {addr:#x}", name)
        for f in own.funcs:
            high: dict[int, int] = {}
            for addr in range(f.start, f.end, 4):
                word = img.word(addr)
                op, rs, rt, imm = word >> 26, (word >> 21) & 31, (word >> 16) & 31, word & 0xFFFF
                if op == 15:
                    high[rt] = imm << 16
                elif op in (9, 13) and rs in high:
                    low = imm - 0x10000 if op == 9 and imm & 0x8000 else imm
                    consider((high[rs] + low) & 0xFFFFFFFF, f"{name} code at {addr:#x}", name)
    for (name, value), where in sorted(hits.items()):
        r.problems.append(f"{name}: {value:#x} follows a jr ra inside {where}: a merged function?")
    return r


def check_recomp_config(image: str, cfg: dict, idx: Index, calls: RuntimeCalls | None) -> Report:
    r = Report()
    general = cfg.get("general", {})
    seen: dict[int, str] = {}
    for sel in general.get("stubs", []):
        m = re.fullmatch(r"([A-Za-z_]\w*)@(0x[0-9A-Fa-f]+)", sel)
        if not m:
            r.problems.append(f"{image}: stub selector {sel!r} is not handler@0xADDR")
            continue
        handler, addr = m.group(1), int(m.group(2), 16)
        if addr in seen:
            r.problems.append(f"{image}: {addr:#x} bound twice ({seen[addr]}, {handler})")
        seen[addr] = handler
        if addr not in idx.start_set:
            r.problems.append(f"{image}: stub {sel} is not at a function start")
        if calls:
            res = calls.resolve(handler)
            if res is None:
                r.problems.append(f"{image}: stub {sel}: the runtime has no handler {handler}")
            elif res[0] == "stub" and res[1] in calls.todo:
                r.problems.append(f"{image}: stub {sel}: the runtime's {res[1]} is a TODO that throws")
    for key in cfg.get("mmio", {}):
        addr = int(key, 0)
        if idx.containing(addr) is None:
            r.problems.append(f"{image}: mmio site {addr:#x} is outside every function")
    r.notes.append(f"{image}: {len(seen)} SDK bindings, {len(cfg.get('mmio', {}))} MMIO sites")
    return r
