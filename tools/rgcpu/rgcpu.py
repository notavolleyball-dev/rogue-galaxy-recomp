#!/usr/bin/env python3
"""Native CPU translator: R5900 functions to C++ on the native contract.

usage: rgcpu.py ELF FUNCTIONS.csv GENERATED_DIR OUT_DIR

Reads each function's instructions from the ELF (bounds from the map) and
writes OUT_DIR/nc_*.cpp: one native function per translated guest function,
with guest registers as C++ locals (docs/NATIVE-CPU.md). GENERATED_DIR is
ps2_recomp's output for the same image; only its file names are used, to
name each native entry NAME_nc after the oracle function NAME it replaces.

Calls between translated functions are direct C calls with signatures from
the whole-program analysis (analysis.py). Anything it does not translate
stays on ps2_recomp's output, and OUT_DIR/skipped.txt says why.
"""
from __future__ import annotations

import csv
import re
import struct
import sys
from pathlib import Path

from analysis import FA, SREGS, BOUNDARY_SAVED, analyse, frame_plans
from r5900 import Insn, Unsupported, decode_any

# Locations in the order the wrapper copies them.
GPRS = [f'r{n}' for n in range(1, 32)]
UPPERS = [f'h{n}' for n in range(1, 32)]
FPRS = [f'f{n}' for n in range(32)]
SCALARS = ['fcr31', 'hi', 'lo', 'hi1', 'lo1', 'sa', 'facc']
CTX_FIELD = {'fcr31': 'fcr31', 'hi': 'hi', 'lo': 'lo', 'hi1': 'hi1', 'lo1': 'lo1', 'sa': 'sa', 'facc': 'f_acc'}


class Image:
    def __init__(self, path: Path):
        elf = path.read_bytes()
        phoff = struct.unpack_from('<I', elf, 0x1C)[0]
        phentsize, phnum = struct.unpack_from('<HH', elf, 0x2A)
        for k in range(phnum):
            p_type, off, vaddr, _pa, filesz, _ms = struct.unpack_from('<6I', elf, phoff + k * phentsize)
            if p_type == 1 and filesz:
                self.base, self.data = vaddr, elf[off:off + filesz]
                return
        raise ValueError('no PT_LOAD')

    def word(self, addr: int) -> int:
        return struct.unpack_from('<I', self.data, addr - self.base)[0]


class Function:
    def __init__(self, name: str, start: int, end: int, stub: bool = False):
        self.name, self.start, self.end = name, start, end
        # ps2_recomp replaced it with a host implementation (ps2_stubs:: or
        # ps2_syscalls::): its MIPS code never runs, calls to it are
        # runtime boundaries.
        self.stub = stub
        # The game replaces its table slot with host code (a hook, listed in
        # config/<disc>/native-hooks.txt): its MIPS code is translated, but a
        # call to it is a boundary, since the hook may read any argument
        # register and write any result one.
        self.hooked = False
        self.insns: list[Insn] = []

    @property
    def external(self) -> bool:
        """Called as a runtime boundary, not through its native signature."""
        return self.stub or self.hooked


def load_functions(csv_path: Path, generated: Path) -> list[Function]:
    oracle = {}
    for f in generated.glob('*_0x*.cpp'):
        m = re.match(r'(.*)_0x([0-9a-f]+)$', f.stem)
        if m:
            oracle[int(m.group(2), 16)] = f.stem
    funcs = []
    with open(csv_path, newline='') as fh:
        rows = csv.reader(fh)
        next(rows)
        for name, start, end, _size in rows:
            s, e = int(start, 16), int(end, 16)
            if s in oracle:
                text = (generated / f'{oracle[s]}.cpp').read_text()
                stub = re.search(r'^\s*// 0x[0-9a-f]+: 0x', text, re.M) is None
                funcs.append(Function(oracle[s], s, e, stub))
    return funcs


ORACLE_INSN = re.compile(r'^\s*// 0x([0-9a-f]+): 0x[0-9a-f]+\s+(\S+)')
NC_FIELDS = {'f', 'fcr31', 'hi', 'lo', 'hi1', 'lo1', 'sa', 'f_acc'}


def oracle_statements(path: Path) -> dict[int, str]:
    """ps2_recomp's statement for each instruction of a function: the line
    after its comment, past the pc bookkeeping."""
    lines = path.read_text().split('\n')
    out = {}
    for k, line in enumerate(lines):
        m = ORACLE_INSN.match(line)
        if not m:
            continue
        j = k + 1
        while j < len(lines) and lines[j].strip().startswith(('ctx->pc =', 'ctx->in_delay_slot', 'ctx->branch_pc')):
            j += 1
        stmt = lines[j].strip() if j < len(lines) else ''
        out[int(m.group(1), 16)] = '' if stmt.startswith('//') else stmt
    return out


def oracle_jump_tables(path: Path) -> dict[int, list[int]]:
    """For each `jr` through a register, the case targets of the switch
    ps2_recomp emitted for it (its jump-table analysis)."""
    lines = path.read_text().split('\n')
    out = {}
    for k, line in enumerate(lines):
        m = ORACLE_INSN.match(line)
        if not m or m.group(2) != 'jr' or '$ra' in line:
            continue
        targets = []
        for l2 in lines[k + 1:k + 4000]:
            if ORACLE_INSN.match(l2) or l2.startswith('label_'):
                break
            c = re.match(r'\s*case 0x([0-9A-Fa-f]+)u: goto', l2)
            if c:
                targets.append(int(c.group(1), 16))
        if targets:
            out[int(m.group(1), 16)] = targets
    return out


def adopt_oracle(ins: Insn, stmt: str) -> bool:
    """Take ps2_recomp's statement for a VU0 or parallel (MMI) instruction.

    VU0 state has no other owner, so it stays in the guest context: those
    accesses go to gctx; GPRs and the FPU stay locals through the macros.
    """
    word = ins.word
    op, rs = word >> 26, (word >> 21) & 31
    vu0 = op == 18 and (rs >= 16 or rs in (1, 2, 5, 6)) or op in (54, 62)
    mmi = op == 28
    if not (vu0 or mmi) or stmt.startswith(('if', 'goto', '{ const bool')):
        return False
    s = re.sub(r'\bctx->(vu0_\w+|vi\b)', r'gctx->\1', stmt)
    s = s.replace('PS2_VU0_FMAC_FLAGS(ctx,', 'PS2_VU0_FMAC_FLAGS(gctx,')
    s = s.replace('runtime->executeVU0Microprogram(rdram, ctx,', 'runtime->executeVU0Microprogram(rdram, gctx,')
    rest = set(re.findall(r'\bctx->(\w+)', s)) - NC_FIELDS
    calls = set(re.findall(r'runtime->(\w+)', s)) - {'executeVU0Microprogram'}
    if rest or calls or 'return' in s:
        return False
    ins.name, ins.kind, ins.code = 'vu0' if vu0 else 'mmi', 'op', s
    return True


def is_lo(loc: str) -> bool:
    return loc[0] == 'r' and loc[1:].isdigit()


def is_up(loc: str) -> bool:
    return loc[0] == 'h' and loc[1:].isdigit()


def ctype(loc: str) -> str:
    if loc[0] == 'f' and loc[1:].isdigit() or loc == 'facc':
        return 'float'
    if loc in ('fcr31', 'sa'):
        return 'uint32_t'
    return 'uint64_t'


def local(loc: str) -> str:
    """The C++ lvalue a location lives in inside a native body."""
    if is_lo(loc) or is_up(loc):
        return f'nc_{loc}'
    if loc[0] == 'f' and loc[1:].isdigit():
        return f'nc.f[{loc[1:]}]'
    return f'nc.{CTX_FIELD[loc]}'


def order(locs) -> list[str]:
    rank = {l: k for k, l in enumerate(GPRS + UPPERS + FPRS + SCALARS)}
    return sorted(locs, key=lambda l: rank[l])


def to_gctx(loc: str, value: str, has_upper: bool) -> str:
    """Store a location into the guest context (a GPR's low half keeps the
    upper half there unless the upper half is stored too)."""
    if is_lo(loc):
        n = loc[1:]
        if has_upper:
            return f'gctx->r[{n}] = _mm_set_epi64x((long long)nc_h{n}, (long long){value});'
        return (f'gctx->r[{n}] = _mm_castpd_si128(_mm_move_sd(_mm_castsi128_pd(gctx->r[{n}]), '
                f'_mm_castsi128_pd(_mm_cvtsi64_si128((long long){value}))));')
    if is_up(loc):
        return ''  # stored with its low half
    if loc[0] == 'f' and loc[1:].isdigit():
        return f'gctx->f[{loc[1:]}] = {value};'
    return f'gctx->{CTX_FIELD[loc]} = {value};'


def from_gctx(loc: str) -> str:
    if is_lo(loc):
        return f'(uint64_t)_mm_cvtsi128_si64(gctx->r[{loc[1:]}])'
    if is_up(loc):
        return f'(uint64_t)_mm_extract_epi64(gctx->r[{loc[1:]}], 1)'
    if loc[0] == 'f' and loc[1:].isdigit():
        return f'gctx->f[{loc[1:]}]'
    return f'gctx->{CTX_FIELD[loc]}'


def store_set(locs) -> list[str]:
    """Statements storing locals `locs` into the guest context."""
    out = []
    for loc in order(locs):
        if is_up(loc):
            if f'r{loc[1:]}' not in locs:
                n = loc[1:]
                out.append(f'gctx->r[{n}] = _mm_set_epi64x((long long)nc_h{n}, _mm_cvtsi128_si64(gctx->r[{n}]));')
            continue
        out.append(to_gctx(loc, local(loc), f'h{loc[1:]}' in locs if is_lo(loc) else False))
    return out


def signature(fn, A) -> tuple[str, str]:
    """(return struct, parameter list) declarations for a native function."""
    a = fn.start
    outs = order(A['OUT'][a])
    ins = order(A['IN'][a])
    fields = ' '.join(f'{ctype(l)} {l};' for l in outs) or 'char unused;'
    struct = f'struct NcO_{a:x} {{ {fields} }};'
    params = ''.join(f', {ctype(l)} p_{l}' for l in ins)
    return struct, f'NcO_{a:x} nc_{a:x}(uint8_t* rdram, R5900Context* gctx, PS2Runtime* runtime{params})'


FRAME_T = {'8': 'uint8_t', '16': 'uint16_t', '32': 'uint32_t', '64': 'uint64_t', '128': '__m128i'}
SP_READ = re.compile(r'READ(8|16|32|64|128)\(ADD32\(GPR_U32\(ctx, 29\), (\d+)\)\)')
SP_WRITE = re.compile(r'WRITE(8|16|32|64|128)\(ADD32\(GPR_U32\(ctx, 29\), (\d+)\), ')


def redirect_to_frame(code: str, idx: int) -> str:
    """A stack access of ours: the guest address becomes the frame array."""
    code, nr = SP_READ.subn(lambda m: f'ncFrameRead<{FRAME_T[m.group(1)]}>(nc_frame, {idx})', code)
    code, nw = SP_WRITE.subn(lambda m: f'ncFrameWrite<{FRAME_T[m.group(1)]}>(nc_frame, {idx}, ', code)
    assert nr + nw == 1, code
    return code


def translate(fn: Function, fa, A, native: dict, index: dict, frame=None) -> str:
    """The C++ for one function on the native contract, or Unsupported.

    `native` maps start address -> Function for every function translated
    (callees called directly); `index` maps a start address to its slot in
    g_ps2DirectCallTable.
    """
    if fn.stub:
        raise Unsupported('host stub in ps2_recomp')
    if fa.problems:
        raise Unsupported(fa.problems[0])
    insns = fa.insns
    for ins in insns:
        if ins.name == 'unsupported':
            raise Unsupported(ins.code)
        if ins.kind == 'unsupported-link':
            raise Unsupported('branch and link')
    a = fn.start
    IN, OUT = A['IN'], A['OUT']

    used = set(IN[a]) | set(OUT[a])
    for ins in insns:
        used |= ins.reads | ins.writes
    for node, callee in fa.callee.items():
        if callee is not None and not callee.external and callee.start in IN:
            used |= IN[callee.start] | OUT[callee.start]
    used -= {'r0', 'h0'}
    has_boundary = any(callee is None or callee.start not in native for callee in fa.callee.values()) \
        or any(ins.kind in ('syscall', 'break') for ins in insns)

    out = []
    w = out.append
    struct, sig = signature(fn, A)
    w(f'{sig} {{')
    w('    const uint64_t nc_r0 = 0, nc_h0 = 0; (void)nc_r0; (void)nc_h0;')
    w('    NcCtx nc;')
    w('    NcCtx* const ctx = &nc;')
    for loc in order(used):
        init = f'p_{loc}' if loc in IN[a] else '0'
        if is_lo(loc) or is_up(loc):
            w(f'    uint64_t {local(loc)} = {init};')
        else:
            w(f'    {local(loc)} = {init};')
    # Boundaries hand over everything but the callee-saved registers the
    # callee gives back untouched (analysis.BOUNDARY_SAVED).
    crossing = set(used) - BOUNDARY_SAVED
    flush = store_set(crossing)
    reload = [f'{local(l)} = {from_gctx(l)};' for l in order(crossing)]
    FLUSH = ' '.join(flush)
    RELOAD = ' '.join(reload)

    if frame is not None and frame.ok:
        w(f'    alignas(16) uint8_t nc_frame[{frame.size}];')
        slot_at = {fa.insns[n].addr: off - frame.lo for n, off in frame.slot.items()}
    else:
        slot_at = {}

    def body(ins: Insn) -> str:
        if ins.kind == 'nop':
            return ''
        if ins.addr in slot_at:
            return redirect_to_frame(ins.code, slot_at[ins.addr])
        return ins.code

    def goto(src: Insn, target: int) -> str:
        back = 'NC_BACKEDGE(); ' if target <= src.addr else ''
        return f'{back}goto L_{target:x};'

    def native_call(callee, ret_expr: str | None) -> str:
        """A call to a known function `callee` (ra already set). Ours and
        its table slot still holds our entry (RG_NATIVE_CPU puts NAME_nc
        there; a hook replaces it): a direct call. Otherwise its trampoline,
        which takes the same arguments and goes through the runtime."""
        c = callee.start
        args = ''.join(f', {local(l)}' for l in order(IN[c]))
        assign = ' '.join(f'{local(l)} = o.{l};' for l in order(OUT[c]))
        if c in native and c in index:
            pick = (f'__builtin_expect(g_ps2DirectCallTable[{index[c]}] == &{callee.name}_nc, 1) ? '
                    f'nc_{c:x}(rdram, gctx, runtime{args}) : ncv_{c:x}(rdram, gctx, runtime{args})')
        else:
            pick = f'ncv_{c:x}(rdram, gctx, runtime{args})'
        return f'NC_CALLCHECK(); {{ const NcO_{c:x} o = {pick}; {assign} }}'

    k = 0
    n = len(insns)
    while k < n:
        ins = insns[k]
        if ins.addr in fa.labels:
            w(f'L_{ins.addr:x}:')
        if ins.kind in ('op', 'nop'):
            c = body(ins)
            if c:
                w(f'    {c}')
            k += 1
            continue
        if ins.kind == 'syscall':
            w(f'    {{ {FLUSH} gctx->pc = 0x{ins.addr + 4:x}u; runtime->handleSyscall(rdram, gctx, {ins.code}); {RELOAD} }}')
            k += 1
            continue
        if ins.kind == 'break':
            w(f'    {{ {FLUSH} gctx->pc = 0x{ins.addr:x}u; runtime->handleBreak(rdram, gctx); {RELOAD} }}')
            k += 1
            continue
        delay = insns[k + 1]
        d = body(delay)
        ret = ins.addr + 8
        if ins.kind == 'branch':
            if ins.likely:
                w(f'    if ({ins.cond}) {{ {d} {goto(ins, ins.target)} }}')
            else:
                w(f'    {{ const bool nc_c = {ins.cond}; {d} if (nc_c) {{ {goto(ins, ins.target)} }} }}')
        elif ins.kind == 'jump' and ins.target in fa.at:
            w(f'    {d} {goto(ins, ins.target)}')
        elif ins.kind == 'jump':
            callee = fa.callee[('tail', k)]
            if callee is None or callee.external:
                w(f'    {d} {{ {FLUSH} NC_CALLCHECK(); ncBoundaryJump(rdram, gctx, runtime, 0x{ins.target:x}u, (uint32_t)nc_r31); {RELOAD} }} goto nc_exit;')
            else:
                w(f'    {d} {native_call(callee, "(uint32_t)nc_r31")} goto nc_exit;')
        elif ins.kind == 'call':
            callee = fa.callee[('call', k)]
            w(f'    SET_GPR_U32(ctx, 31, 0x{ret:x}u); {d}')
            if callee is None or callee.external:
                w(f'    NC_CALLCHECK(); {{ {FLUSH} ncBoundaryCall(rdram, gctx, runtime, 0x{ins.target:x}u, 0x{ret:x}u); {RELOAD} }}')
            else:
                w(f'    {native_call(callee, f"0x{ret:x}u")}')
        elif ins.kind == 'jalr':
            w(f'    {{ const uint32_t nc_t = GPR_U32(ctx, {ins.rs}); SET_GPR_U32(ctx, {ins.rd}, 0x{ret:x}u); {d}')
            w(f'      NC_CALLCHECK(); {FLUSH} ncBoundaryCall(rdram, gctx, runtime, nc_t, 0x{ret:x}u); {RELOAD} }}')
        elif ins.kind == 'jr' and ins.rs == 31:
            w(f'    {d} goto nc_exit;')
        elif ins.kind == 'jr':
            cases = ' '.join(f'case 0x{t:x}u: goto L_{t:x};' for t in ins.targets if t in fa.at)
            w(f'    {{ const uint32_t nc_t = GPR_U32(ctx, {ins.rs}); {d}')
            w(f'      switch (nc_t) {{ {cases} default: break; }}')
            w(f'      NC_CALLCHECK(); {FLUSH} ncBoundaryJump(rdram, gctx, runtime, nc_t, (uint32_t)nc_r31); {RELOAD} goto nc_exit; }}')
        else:
            raise AssertionError(ins.kind)
        if delay.addr in fa.labels:
            w(f'    goto L_{delay.addr:x}_past;')
            w(f'L_{delay.addr:x}:')
            if d:
                w(f'    {d}')
            w(f'L_{delay.addr:x}_past:;')
        k += 2

    w('nc_exit:')
    outs = order(OUT[a])
    vals = ', '.join(local(l) for l in outs)
    w(f'    return NcO_{a:x}{{ {vals} }};' if outs else f'    return NcO_{a:x}{{}};')
    w('}')

    # The entry from the guest context: the function table, threads, jalr,
    # the old code's calls.
    ins_ = order(IN[a])
    args = ''.join(f', {from_gctx(l)}' for l in ins_)
    w(f'void {fn.name}_nc(uint8_t* rdram, R5900Context* gctx, PS2Runtime* runtime) {{')
    w(f'    const uint32_t nc_ra = (uint32_t)_mm_cvtsi128_si64(gctx->r[31]);')
    # A boundary call anywhere below stores the native frames' registers
    # into gctx; what this function gives back unchanged goes back in place.
    pres = A['PRES'][a]
    keep_r = [n for n in SREGS if f'r{n}' in pres or f'h{n}' in pres]
    keep_f = [n for n in range(20, 32) if f'f{n}' in pres]
    for n in keep_r:
        w(f'    const __m128i nc_s{n} = gctx->r[{n}];')
    for n in keep_f:
        w(f'    const float nc_sf{n} = gctx->f[{n}];')
    w(f'    const NcO_{a:x} o = nc_{a:x}(rdram, gctx, runtime{args});')
    for n in keep_r:
        if f'r{n}' in pres and f'h{n}' in pres:
            w(f'    gctx->r[{n}] = nc_s{n};')
        elif f'r{n}' in pres:
            w(f'    gctx->r[{n}] = _mm_castpd_si128(_mm_move_sd(_mm_castsi128_pd(gctx->r[{n}]), _mm_castsi128_pd(nc_s{n})));')
        else:
            w(f'    gctx->r[{n}] = _mm_castpd_si128(_mm_move_sd(_mm_castsi128_pd(nc_s{n}), _mm_castsi128_pd(gctx->r[{n}])));')
    for n in keep_f:
        w(f'    gctx->f[{n}] = nc_sf{n};')
    for st in store_set(set(outs)):
        st = re.sub(r'nc_([rh]\d+)', r'o.\1', st)
        st = re.sub(r'nc\.f\[(\d+)\]', r'o.f\1', st)
        st = re.sub(r'nc\.(\w+)', lambda m: 'o.' + {v: k for k, v in CTX_FIELD.items()}[m.group(1)], st)
        w(f'    {st}')
    w(f'    gctx->pc = {"(uint32_t)o.r31" if "r31" in OUT[a] else "nc_ra"};')
    w('}')
    text = '\n'.join(out) + '\n'
    text = re.sub(r'SET_GPR_\w+\(ctx, 0, ', 'NC_DISCARD(', text)
    return text


def trampoline(fn, A) -> str:
    """ncv_: a known function called through the guest context -- the
    callee is not native, or a host hook has its table slot. Same arguments
    and results as nc_, from the dataflow."""
    a = fn.start
    _struct, sig = signature(fn, A)
    sig = sig.replace(f' nc_{a:x}(', f' ncv_{a:x}(', 1)
    ins = set(A['IN'][a])
    lines = [f'{sig} {{']
    for st in store_set(ins):
        st = re.sub(r'nc_([rh]\d+)', r'p_\1', st)
        st = re.sub(r'nc\.f\[(\d+)\]', r'p_f\1', st)
        st = re.sub(r'nc\.(\w+)', lambda m: 'p_' + {v: k for k, v in CTX_FIELD.items()}[m.group(1)], st)
        lines.append(f'    {st}')
    ret = '(uint32_t)p_r31' if 'r31' in ins else '(uint32_t)_mm_cvtsi128_si64(gctx->r[31])'
    lines.append(f'    ncBoundaryCall(rdram, gctx, runtime, 0x{a:x}u, {ret});')
    outs = order(A['OUT'][a])
    vals = ', '.join(from_gctx(l) for l in outs)
    lines.append(f'    return NcO_{a:x}{{ {vals} }};')
    lines.append('}')
    return '\n'.join(lines) + '\n'


HEADER = '''// Generated by tools/rgcpu/rgcpu.py: R5900 functions on the native CPU
// contract (docs/NATIVE-CPU.md). Do not edit.
#include "nc_contract.h"
#define NC_DISCARD(v) ((void)(v))
'''


def load_index(generated: Path) -> dict:
    """Start address -> slot in g_ps2DirectCallTable (register_functions.cpp)."""
    text = (generated / 'register_functions.cpp').read_text()
    body = text[text.index('kDirectCallTargets['):]
    body = body[body.index('{') + 1:body.index('};')]
    return {int(v, 16): k for k, v in enumerate(re.findall(r'0x([0-9a-f]+)u', body))}


def main():
    elf, csv_path, generated, out_dir = map(Path, sys.argv[1:5])
    img = Image(elf)
    funcs = load_functions(csv_path, generated)
    hooks_path = csv_path.parent / 'native-hooks.txt'
    if hooks_path.exists():
        hooked = {int(l.split('#')[0], 16) for l in hooks_path.read_text().splitlines() if l.split('#')[0].strip()}
        for f in funcs:
            f.hooked = f.start in hooked
        print(f'{sum(f.hooked for f in funcs)} hooked functions from {hooks_path.name}')
    starts = {f.start: f for f in funcs}
    index = load_index(generated)
    fas = {}
    for f in funcs:
        if f.stub:
            continue
        insns = [decode_any(a, img.word(a)) for a in range(f.start, f.end, 4)]
        if any(i.kind == 'jr' and i.rs != 31 for i in insns):
            tables = oracle_jump_tables(generated / f'{f.name}.cpp')
            for i in insns:
                if i.kind == 'jr' and i.rs != 31 and i.addr in tables:
                    i.targets = tables[i.addr]
        if any(i.name == 'unsupported' for i in insns):
            stmts = oracle_statements(generated / f'{f.name}.cpp')
            for i in insns:
                if i.name == 'unsupported' and i.addr in stmts:
                    adopt_oracle(i, stmts[i.addr])
        fas[f.start] = FA(f, insns, starts)
    A = analyse(fas)
    frames = frame_plans(fas, A)

    # What can be native: no unsupported instruction, no odd control flow.
    native = {}
    reasons = {}
    for a, fa in fas.items():
        bad = fa.problems[:1] or [i.code for i in fa.insns if i.name == 'unsupported'][:1] \
            or ['branch and link' for i in fa.insns if i.kind == 'unsupported-link'][:1]
        if bad:
            reasons[fa.fn.name] = bad[0]
        else:
            native[a] = fa.fn

    out_dir.mkdir(parents=True, exist_ok=True)
    # Each file declares only what it uses: one shared header of every
    # signature was 27 MB, parsed again by each of ~180 files.
    decl = {}
    names = {}
    for a, fa in fas.items():
        f = fa.fn
        struct, sig = signature(f, A)
        d = [struct, sig.replace(f' nc_{a:x}(', f' ncv_{a:x}(', 1) + ';']
        if a in native:
            d += [sig + ';', f'void {f.name}_nc(uint8_t* rdram, R5900Context* gctx, PS2Runtime* runtime);']
        decl[a] = '\n'.join(d)
        names[f.name] = a
    ref = re.compile(r'\b(?:nc|ncv|NcO)_([0-9a-f]+)\b')
    oracle_ref = re.compile(r'&(\w+_0x[0-9a-f]+)\b')

    done, skipped = [], [f'{n}: {r}' for n, r in reasons.items()]
    chunk, n_chunk = [], 0

    def flush():
        nonlocal chunk, n_chunk
        if chunk:
            body = '\n'.join(chunk)
            used = sorted({int(x, 16) for x in ref.findall(body)} & decl.keys())
            oracle = sorted(set(oracle_ref.findall(body)))
            head = [decl[a] for a in used]
            head += [f'void {n}(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);' for n in oracle]
            (out_dir / f'nc_{n_chunk:03d}.cpp').write_text(HEADER + '\n'.join(head) + '\n\n' + body)
            n_chunk += 1
            chunk = []

    for a, fa in fas.items():
        f = fa.fn
        text = trampoline(f, A)
        if a in native:
            text += translate(f, fa, A, native, index, frames[a])
            done.append(f.name)
        chunk.append(text)
        if len(chunk) >= 100:
            flush()
    flush()
    (out_dir / 'names.txt').write_text('\n'.join(done) + '\n')
    # The game's table of them (src/native_cpu.cpp): with RG_NATIVE_CPU=1,
    # each slot still holding the oracle's function gets NAME_nc.
    reg = ['// Generated by tools/rgcpu/rgcpu.py. Do not edit.',
           '#include "ps2_runtime.h"',
           'struct RgNativeEntry { uint32_t address; PS2Runtime::RecompiledFunction oracle, native; bool hooked; };']
    for a in native:
        n = fas[a].fn.name
        reg.append(f'void {n}(uint8_t*, R5900Context*, PS2Runtime*);')
        reg.append(f'void {n}_nc(uint8_t*, R5900Context*, PS2Runtime*);')
    reg.append('extern const RgNativeEntry g_rgNativeEntries[] = {')
    reg += [f'    {{0x{a:x}u, &{fas[a].fn.name}, &{fas[a].fn.name}_nc, {"true" if fas[a].fn.hooked else "false"}}},' for a in sorted(native)]
    reg.append('};')
    reg.append(f'extern const uint32_t g_rgNativeEntryCount = {len(native)}u;')
    (out_dir / 'register_native.cpp').write_text('\n'.join(reg) + '\n')
    (out_dir / 'skipped.txt').write_text('\n'.join(skipped) + '\n')
    (out_dir / 'stubs.txt').write_text(''.join(f'{f.start:x} {f.name}\n' for f in funcs if f.stub))
    (out_dir / 'funcs.txt').write_text(''.join(f'{f.start:x} {f.end:x} {f.name}\n' for f in funcs if not f.stub))
    (out_dir / 'table.txt').write_text(''.join(f'{index[a]} {f.name}\n' for a, f in starts.items()
                                               if a in index and not f.stub))
    owned = sum(1 for a in native if frames[a].ok)
    print(f'frames owned: {owned} of {len(native)} native functions; '
          f'{sum(len(frames[a].slot) for a in native)} stack accesses off guest memory')
    print(f'translated {len(done)} of {len(funcs)} functions into {n_chunk} files '
          f'({A["rounds"]} dataflow rounds)')


if __name__ == '__main__':
    main()
