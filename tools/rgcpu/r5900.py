"""R5900 decoding and per-instruction C++ for the native CPU translator.

Each instruction becomes the same C++ expression ps2_recomp emits for it, so
results match the oracle bit for bit, but in terms of the macros
tools/rgcpu/nc_contract.h maps onto locals (GPR_*/SET_GPR_* with a
literal register number, ctx->f[n], ctx->lo, ...). Control flow, delay
slots, calls and everything that touches the runtime are the translator's
own (rgcpu.py); this module only knows single instructions.

Locations, the units the dataflow in rgcpu.py tracks:
  r1..r31 low 64 bits of a GPR     h1..h31 its upper 64 bits
  f0..f31 FPU registers            fcr31, hi, lo, hi1, lo1, sa, facc
"""
from __future__ import annotations

from dataclasses import dataclass, field


class Unsupported(Exception):
    """An instruction the translator leaves to ps2_recomp's output."""


def s16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def u32(v: int) -> int:
    return v & 0xFFFFFFFF


@dataclass
class Insn:
    addr: int
    word: int
    name: str = ''
    kind: str = 'op'  # op, branch, jump, call, jr, jalr, syscall, break, nop
    likely: bool = False
    cond: str = ''  # branch condition (C++)
    target: int | None = None
    link: bool = False  # bltzal & co. (unsupported) / jal / jalr
    reads: set = field(default_factory=set)
    writes: set = field(default_factory=set)
    code: str = ''  # the statement(s) for op instructions
    mem: bool = False
    rs: int = 0
    rt: int = 0
    rd: int = 0
    targets: list | None = None  # a jump table's targets (jr through a register)


def G32(r):
    return f'GPR_U32(ctx, {r})'


def S32(r):
    return f'GPR_S32(ctx, {r})'


def G64(r):
    return f'GPR_U64(ctx, {r})'


def S64(r):
    return f'GPR_S64(ctx, {r})'


def GV(r):
    return f'GPR_VEC(ctx, {r})'


def lo(r):
    return {f'r{r}'} if r else set()


def hi(r):
    return {f'h{r}'} if r else set()


def addr_expr(base: int, imm: int) -> str:
    return f'ADD32({G32(base)}, {u32(s16(imm))})'


def decode(addr: int, word: int) -> Insn:
    """One instruction. Raises Unsupported for what the translator refuses."""
    op = word >> 26
    rs = (word >> 21) & 31
    rt = (word >> 16) & 31
    rd = (word >> 11) & 31
    sa = (word >> 6) & 31
    fn = word & 63
    imm = word & 0xFFFF
    i = Insn(addr, word, rs=rs, rt=rt, rd=rd)
    if word == 0:
        i.name, i.kind = 'nop', 'nop'
        return i

    def op3(name, code, reads, writes):
        i.name, i.code = name, code
        i.reads |= reads
        i.writes |= writes
        return i

    def branch(name, cond, reads, likely=False):
        i.name, i.kind, i.cond, i.likely = name, 'branch', cond, likely
        i.target = u32(addr + 4 + (s16(imm) << 2))
        i.reads |= reads
        return i

    if op == 0:  # SPECIAL
        if fn == 0:
            return op3('sll', f'SET_GPR_S32(ctx, {rd}, (int32_t)SLL32({G32(rt)}, {sa}));', lo(rt), lo(rd))
        if fn == 2:
            return op3('srl', f'SET_GPR_S32(ctx, {rd}, (int32_t)SRL32({G32(rt)}, {sa}));', lo(rt), lo(rd))
        if fn == 3:
            return op3('sra', f'SET_GPR_S32(ctx, {rd}, SRA32({S32(rt)}, {sa}));', lo(rt), lo(rd))
        if fn == 4:
            return op3('sllv', f'SET_GPR_S32(ctx, {rd}, (int32_t)SLL32({G32(rt)}, {G32(rs)} & 0x1F));', lo(rt) | lo(rs), lo(rd))
        if fn == 6:
            return op3('srlv', f'SET_GPR_S32(ctx, {rd}, (int32_t)SRL32({G32(rt)}, {G32(rs)} & 0x1F));', lo(rt) | lo(rs), lo(rd))
        if fn == 7:
            return op3('srav', f'SET_GPR_S32(ctx, {rd}, SRA32({S32(rt)}, {G32(rs)} & 0x1F));', lo(rt) | lo(rs), lo(rd))
        if fn == 8:
            i.name, i.kind = 'jr', 'jr'
            i.reads |= lo(rs)
            return i
        if fn == 9:
            i.name, i.kind, i.link = 'jalr', 'jalr', True
            i.reads |= lo(rs)
            i.writes |= lo(rd)
            return i
        if fn == 10:
            return op3('movz', f'if ({G64(rt)} == 0) SET_GPR_VEC(ctx, {rd}, {GV(rs)});', lo(rt) | lo(rs) | hi(rs) | lo(rd) | hi(rd), lo(rd) | hi(rd))
        if fn == 11:
            return op3('movn', f'if ({G64(rt)} != 0) SET_GPR_VEC(ctx, {rd}, {GV(rs)});', lo(rt) | lo(rs) | hi(rs) | lo(rd) | hi(rd), lo(rd) | hi(rd))
        if fn == 12:
            i.name, i.kind = 'syscall', 'syscall'
            i.code = f'0x{(word >> 6) & 0xFFFFF:x}u'
            return i
        if fn == 13:
            i.name, i.kind = 'break', 'break'
            return i
        if fn == 15:
            i.name, i.kind = 'sync', 'nop'
            return i
        if fn == 16:
            return op3('mfhi', f'SET_GPR_U64(ctx, {rd}, ctx->hi);', {'hi'}, lo(rd))
        if fn == 17:
            return op3('mthi', f'ctx->hi = {G64(rs)};', lo(rs), {'hi'})
        if fn == 18:
            return op3('mflo', f'SET_GPR_U64(ctx, {rd}, ctx->lo);', {'lo'}, lo(rd))
        if fn == 19:
            return op3('mtlo', f'ctx->lo = {G64(rs)};', lo(rs), {'lo'})
        if fn == 20:
            return op3('dsllv', f'SET_GPR_U64(ctx, {rd}, {G64(rt)} << ({G32(rs)} & 0x3F));', lo(rt) | lo(rs), lo(rd))
        if fn == 22:
            return op3('dsrlv', f'SET_GPR_U64(ctx, {rd}, {G64(rt)} >> ({G32(rs)} & 0x3F));', lo(rt) | lo(rs), lo(rd))
        if fn == 23:
            return op3('dsrav', f'SET_GPR_S64(ctx, {rd}, {S64(rt)} >> ({G32(rs)} & 0x3F));', lo(rt) | lo(rs), lo(rd))
        if fn == 24:
            return op3('mult', f'{{ int64_t result = (int64_t){S32(rs)} * (int64_t){S32(rt)}; ctx->lo = (uint64_t)(int64_t)(int32_t)result; ctx->hi = (uint64_t)(int64_t)(int32_t)(result >> 32);' + (f' SET_GPR_S32(ctx, {rd}, (int32_t)result);' if rd else '') + ' }',
                       lo(rs) | lo(rt), {'hi', 'lo'} | lo(rd))
        if fn == 25:
            return op3('multu', f'{{ uint64_t result = (uint64_t){G32(rs)} * (uint64_t){G32(rt)}; ctx->lo = (uint64_t)(int64_t)(int32_t)result; ctx->hi = (uint64_t)(int64_t)(int32_t)(result >> 32);' + (f' SET_GPR_S32(ctx, {rd}, (int32_t)result);' if rd else '') + ' }',
                       lo(rs) | lo(rt), {'hi', 'lo'} | lo(rd))
        if fn == 26:
            return op3('div', f'{{ int32_t divisor = {S32(rt)}; int32_t dividend = {S32(rs)}; if (divisor != 0) {{ if (divisor == -1 && dividend == INT32_MIN) {{ ctx->lo = (uint64_t)(int64_t)INT32_MIN; ctx->hi = 0; }} else {{ ctx->lo = (uint64_t)(int64_t)(dividend / divisor); ctx->hi = (uint64_t)(int64_t)(dividend % divisor); }} }} else {{ ctx->lo = (dividend < 0) ? 1ull : 0xFFFFFFFFFFFFFFFFull; ctx->hi = (uint64_t)(int64_t)dividend; }} }}',
                       lo(rs) | lo(rt), {'hi', 'lo'})
        if fn == 27:
            return op3('divu', f'{{ uint32_t divisor = {G32(rt)}; if (divisor != 0) {{ ctx->lo = (uint64_t)(int64_t)(int32_t)({G32(rs)} / divisor); ctx->hi = (uint64_t)(int64_t)(int32_t)({G32(rs)} % divisor); }} else {{ ctx->lo = 0xFFFFFFFFFFFFFFFFull; ctx->hi = (uint64_t)(int64_t)(int32_t){G32(rs)}; }} }}',
                       lo(rs) | lo(rt), {'hi', 'lo'})
        if fn == 32:
            return op3('add', f'{{ uint32_t tmp; bool ov; ADD32_OV({G32(rs)}, {G32(rt)}, tmp, ov); if (ov) ncOverflow(gctx, runtime); else SET_GPR_S32(ctx, {rd}, (int32_t)tmp); }}', lo(rs) | lo(rt), lo(rd))
        if fn == 33:
            return op3('addu', f'SET_GPR_S32(ctx, {rd}, (int32_t)ADD32({G32(rs)}, {G32(rt)}));', lo(rs) | lo(rt), lo(rd))
        if fn == 34:
            return op3('sub', f'{{ uint32_t tmp; bool ov; SUB32_OV({G32(rs)}, {G32(rt)}, tmp, ov); if (ov) ncOverflow(gctx, runtime); else SET_GPR_S32(ctx, {rd}, (int32_t)tmp); }}', lo(rs) | lo(rt), lo(rd))
        if fn == 35:
            return op3('subu', f'SET_GPR_S32(ctx, {rd}, (int32_t)SUB32({G32(rs)}, {G32(rt)}));', lo(rs) | lo(rt), lo(rd))
        if fn == 36:
            return op3('and', f'SET_GPR_U64(ctx, {rd}, {G64(rs)} & {G64(rt)});', lo(rs) | lo(rt), lo(rd))
        if fn == 37:
            return op3('or', f'SET_GPR_U64(ctx, {rd}, {G64(rs)} | {G64(rt)});', lo(rs) | lo(rt), lo(rd))
        if fn == 38:
            return op3('xor', f'SET_GPR_U64(ctx, {rd}, {G64(rs)} ^ {G64(rt)});', lo(rs) | lo(rt), lo(rd))
        if fn == 39:
            return op3('nor', f'SET_GPR_U64(ctx, {rd}, ~({G64(rs)} | {G64(rt)}));', lo(rs) | lo(rt), lo(rd))
        if fn == 40:
            return op3('mfsa', f'SET_GPR_U32(ctx, {rd}, ctx->sa);', {'sa'}, lo(rd))
        if fn == 41:
            return op3('mtsa', f'ctx->sa = {G32(rs)} & 0x7F;', lo(rs), {'sa'})
        if fn == 42:
            return op3('slt', f'SET_GPR_U64(ctx, {rd}, ((int64_t){S64(rs)} < (int64_t){S64(rt)}) ? 1 : 0);', lo(rs) | lo(rt), lo(rd))
        if fn == 43:
            return op3('sltu', f'SET_GPR_U64(ctx, {rd}, ((uint64_t){G64(rs)} < (uint64_t){G64(rt)}) ? 1 : 0);', lo(rs) | lo(rt), lo(rd))
        if fn == 45:
            return op3('daddu', f'SET_GPR_U64(ctx, {rd}, (uint64_t){G64(rs)} + (uint64_t){G64(rt)});', lo(rs) | lo(rt), lo(rd))
        if fn == 47:
            return op3('dsubu', f'SET_GPR_U64(ctx, {rd}, {G64(rs)} - {G64(rt)});', lo(rs) | lo(rt), lo(rd))
        if fn == 56:
            return op3('dsll', f'SET_GPR_U64(ctx, {rd}, {G64(rt)} << {sa});', lo(rt), lo(rd))
        if fn == 58:
            return op3('dsrl', f'SET_GPR_U64(ctx, {rd}, {G64(rt)} >> {sa});', lo(rt), lo(rd))
        if fn == 59:
            return op3('dsra', f'SET_GPR_S64(ctx, {rd}, {S64(rt)} >> {sa});', lo(rt), lo(rd))
        if fn == 60:
            return op3('dsll32', f'SET_GPR_U64(ctx, {rd}, {G64(rt)} << (32 + {sa}));', lo(rt), lo(rd))
        if fn == 62:
            return op3('dsrl32', f'SET_GPR_U64(ctx, {rd}, {G64(rt)} >> (32 + {sa}));', lo(rt), lo(rd))
        if fn == 63:
            return op3('dsra32', f'SET_GPR_S64(ctx, {rd}, {S64(rt)} >> (32 + {sa}));', lo(rt), lo(rd))
        raise Unsupported(f'special fn {fn}')

    if op == 1:  # REGIMM
        conds = {0: ('bltz', '<'), 1: ('bgez', '>='), 2: ('bltzl', '<'), 3: ('bgezl', '>=')}
        if rt in conds:
            name, rel = conds[rt]
            return branch(name, f'({S64(rs)} {rel} 0)', lo(rs), likely=rt >= 2)
        if rt == 24:
            return op3('mtsab', f'ctx->sa = (({G32(rs)} ^ (uint32_t){imm}) & 0xF) << 3;', lo(rs), {'sa'})
        if rt == 25:
            return op3('mtsah', f'ctx->sa = (({G32(rs)} ^ (uint32_t){imm}) & 0x7) << 4;', lo(rs), {'sa'})
        raise Unsupported(f'regimm rt {rt}')

    if op in (2, 3):
        i.name = 'j' if op == 2 else 'jal'
        i.kind = 'jump' if op == 2 else 'call'
        i.target = ((addr + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
        if op == 3:
            i.link = True
            i.writes |= {'r31'}
        return i
    if op in (4, 5, 20, 21):
        rel = '==' if op in (4, 20) else '!='
        return branch(('beq', 'bne', 'beql', 'bnel')[(op & 1) + (2 if op >= 20 else 0)],
                      f'({G64(rs)} {rel} {G64(rt)})', lo(rs) | lo(rt), likely=op >= 20)
    if op in (6, 7, 22, 23):
        rel = '<=' if op in (6, 22) else '>'
        return branch(('blez', 'bgtz', 'blezl', 'bgtzl')[(op & 1) + (2 if op >= 22 else 0)],
                      f'({S64(rs)} {rel} 0)', lo(rs), likely=op >= 22)
    if op == 8:
        return op3('addi', f'{{ uint32_t tmp; bool ov; ADD32_OV({G32(rs)}, (int32_t){u32(s16(imm))}, tmp, ov); if (ov) ncOverflow(gctx, runtime); else SET_GPR_S32(ctx, {rt}, (int32_t)tmp); }}', lo(rs), lo(rt))
    if op == 9:
        return op3('addiu', f'SET_GPR_S32(ctx, {rt}, (int32_t)ADD32({G32(rs)}, {u32(s16(imm))}));', lo(rs), lo(rt))
    if op == 10:
        return op3('slti', f'SET_GPR_U64(ctx, {rt}, ((int64_t){S64(rs)} < (int64_t)(int32_t){s16(imm)}) ? 1 : 0);', lo(rs), lo(rt))
    if op == 11:
        return op3('sltiu', f'SET_GPR_U64(ctx, {rt}, ((uint64_t){G64(rs)} < (uint64_t)(int64_t)(int32_t){s16(imm)}) ? 1 : 0);', lo(rs), lo(rt))
    if op == 12:
        return op3('andi', f'SET_GPR_U64(ctx, {rt}, {G64(rs)} & (uint64_t)(uint16_t){imm});', lo(rs), lo(rt))
    if op == 13:
        return op3('ori', f'SET_GPR_U64(ctx, {rt}, {G64(rs)} | (uint64_t)(uint16_t){imm});', lo(rs), lo(rt))
    if op == 14:
        return op3('xori', f'SET_GPR_U64(ctx, {rt}, {G64(rs)} ^ (uint64_t)(uint16_t){imm});', lo(rs), lo(rt))
    if op == 15:
        return op3('lui', f'SET_GPR_S32(ctx, {rt}, (int32_t)((uint32_t){imm} << 16));', set(), lo(rt))
    if op == 25:
        return op3('daddiu', f'SET_GPR_S64(ctx, {rt}, (int64_t){S64(rs)} + (int64_t)(int32_t){s16(imm)});', lo(rs), lo(rt))

    if op == 17:  # COP1
        ft, fs, fd = rt, rd, sa
        if rs == 0:
            return op3('mfc1', f'{{ uint32_t bits; std::memcpy(&bits, &ctx->f[{fs}], sizeof(bits)); SET_GPR_U32(ctx, {rt}, bits); }}', {f'f{fs}'}, lo(rt))
        if rs == 4:
            return op3('mtc1', f'{{ uint32_t bits = {G32(rt)}; std::memcpy(&ctx->f[{fs}], &bits, sizeof(bits)); }}', lo(rt), {f'f{fs}'})
        if rs == 6 and fs == 31:
            return op3('ctc1', f'ctx->fcr31 = {G32(rt)} & 0x0183FFFF;', lo(rt), {'fcr31'})
        if rs == 8:
            bc = {0: ('bc1f', '!', False), 1: ('bc1t', '', False), 2: ('bc1fl', '!', True), 3: ('bc1tl', '', True)}
            if rt not in bc:
                raise Unsupported('bc1')
            name, neg, likely = bc[rt]
            return branch(name, f'({neg}(ctx->fcr31 & 0x800000))', {'fcr31'}, likely)
        if rs == 16:  # S
            F = lambda n: f'ctx->f[{n}]'
            r2 = {f'f{fs}', f'f{ft}'}
            if fn == 0:
                return op3('add.s', f'{F(fd)} = FPU_ADD_S({F(fs)}, {F(ft)});', r2, {f'f{fd}'})
            if fn == 1:
                return op3('sub.s', f'{F(fd)} = FPU_SUB_S({F(fs)}, {F(ft)});', r2, {f'f{fd}'})
            if fn == 2:
                return op3('mul.s', f'{F(fd)} = FPU_MUL_S({F(fs)}, {F(ft)});', r2, {f'f{fd}'})
            if fn == 3:
                return op3('div.s', f'if ({F(ft)} == 0.0f) {{ ctx->fcr31 |= 0x100000; /* DZ flag */ {F(fd)} = copysignf(INFINITY, {F(fs)} * 0.0f); }} else {F(fd)} = {F(fs)} / {F(ft)};',
                           r2 | {'fcr31'}, {f'f{fd}', 'fcr31'})
            if fn == 4:
                return op3('sqrt.s', f'{F(fd)} = FPU_SQRT_S({F(ft)});', {f'f{ft}'}, {f'f{fd}'})
            if fn == 5:
                return op3('abs.s', f'{F(fd)} = FPU_ABS_S({F(fs)});', {f'f{fs}'}, {f'f{fd}'})
            if fn == 6:
                return op3('mov.s', f'{F(fd)} = FPU_MOV_S({F(fs)});', {f'f{fs}'}, {f'f{fd}'})
            if fn == 7:
                return op3('neg.s', f'{F(fd)} = FPU_NEG_S({F(fs)});', {f'f{fs}'}, {f'f{fd}'})
            if fn == 24:
                return op3('adda.s', f'FPU_SET_ACC(ctx, FPU_ADD_S({F(fs)}, {F(ft)}));', r2, {'facc'})
            if fn == 26:
                return op3('mula.s', f'FPU_SET_ACC(ctx, FPU_MUL_S({F(fs)}, {F(ft)}));', r2, {'facc'})
            if fn == 28:
                return op3('madd.s', f'{F(fd)} = FPU_ADD_S(ctx->f_acc, FPU_MUL_S({F(fs)}, {F(ft)}));', r2 | {'facc'}, {f'f{fd}'})
            if fn == 29:
                return op3('msub.s', f'{F(fd)} = FPU_SUB_S(ctx->f_acc, FPU_MUL_S({F(fs)}, {F(ft)}));', r2 | {'facc'}, {f'f{fd}'})
            if fn == 30:
                return op3('madda.s', f'FPU_SET_ACC(ctx, FPU_ADD_S(ctx->f_acc, FPU_MUL_S({F(fs)}, {F(ft)})));', r2 | {'facc'}, {'facc'})
            if fn == 31:
                return op3('msuba.s', f'FPU_SET_ACC(ctx, FPU_SUB_S(ctx->f_acc, FPU_MUL_S({F(fs)}, {F(ft)})));', r2 | {'facc'}, {'facc'})
            if fn == 36:
                return op3('cvt.w.s', f'{{ int32_t tmp = FPU_CVT_W_S({F(fs)}); std::memcpy(&{F(fd)}, &tmp, sizeof(tmp)); }}', {f'f{fs}'}, {f'f{fd}'})
            cmp = {50: 'FPU_C_EQ_S', 52: 'FPU_C_OLT_S', 54: 'FPU_C_OLE_S'}
            if fn in cmp:
                return op3('c.s', f'ctx->fcr31 = ({cmp[fn]}({F(fs)}, {F(ft)})) ? (ctx->fcr31 | 0x800000) : (ctx->fcr31 & ~0x800000);', r2 | {'fcr31'}, {'fcr31'})
            raise Unsupported(f'cop1.s fn {fn}')
        if rs == 20 and fn == 32:
            return op3('cvt.s.w', f'{{ int32_t tmp; std::memcpy(&tmp, &ctx->f[{fs}], sizeof(tmp)); ctx->f[{fd}] = FPU_CVT_S_W(tmp); }}', {f'f{fs}'}, {f'f{fd}'})
        raise Unsupported(f'cop1 rs {rs} fn {fn}')

    if op == 28:  # MMI: the scalar ones the game uses
        if fn == 0 or fn == 1:
            signed = fn == 0
            T = 'int64_t' if signed else 'uint64_t'
            rd_set = f' SET_GPR_S32(ctx, {rd}, (int32_t)result);' if rd else ''
            src = (S32, S32) if signed else (G32, G32)
            return op3('madd' if signed else 'maddu',
                       f'{{ uint64_t acc = Ps2HiLoToU64(ctx->hi, ctx->lo); {T} prod = ({T}){src[0](rs)} * ({T}){src[1](rt)}; {T} result = acc + prod; ctx->lo = Ps2SignExt32ToU64((uint32_t)result); ctx->hi = Ps2SignExt32ToU64((uint32_t)(result >> 32));{rd_set} }}',
                       lo(rs) | lo(rt) | {'hi', 'lo'}, {'hi', 'lo'} | lo(rd))
        if fn == 16:
            return op3('mfhi1', f'SET_GPR_U64(ctx, {rd}, ctx->hi1);', {'hi1'}, lo(rd))
        if fn == 17:
            return op3('mthi1', f'ctx->hi1 = {G64(rs)};', lo(rs), {'hi1'})
        if fn == 18:
            return op3('mflo1', f'SET_GPR_U64(ctx, {rd}, ctx->lo1);', {'lo1'}, lo(rd))
        if fn == 19:
            return op3('mtlo1', f'ctx->lo1 = {G64(rs)};', lo(rs), {'lo1'})
        if fn == 24:
            return op3('mult1', f'{{ int64_t result = (int64_t){S32(rs)} * (int64_t){S32(rt)}; ctx->lo1 = (uint64_t)(int64_t)(int32_t)result; ctx->hi1 = (uint64_t)(int64_t)(int32_t)(result >> 32);' + (f' SET_GPR_S32(ctx, {rd}, (int32_t)result);' if rd else '') + ' }',
                       lo(rs) | lo(rt), {'hi1', 'lo1'} | lo(rd))
        if fn == 25:
            return op3('multu1', f'{{ uint64_t result = (uint64_t){G32(rs)} * (uint64_t){G32(rt)}; ctx->lo1 = (uint64_t)(int64_t)(int32_t)result; ctx->hi1 = (uint64_t)(int64_t)(int32_t)(result >> 32);' + (f' SET_GPR_S32(ctx, {rd}, (int32_t)result);' if rd else '') + ' }',
                       lo(rs) | lo(rt), {'hi1', 'lo1'} | lo(rd))
        raise Unsupported(f'mmi fn {fn}')

    # Loads and stores.
    A = addr_expr(rs, imm)
    base = lo(rs)
    i.mem = True
    if op == 32:
        return op3('lb', f'SET_GPR_S32(ctx, {rt}, (int8_t)READ8({A}));', base, lo(rt))
    if op == 33:
        return op3('lh', f'SET_GPR_S32(ctx, {rt}, (int16_t)READ16({A}));', base, lo(rt))
    if op == 35:
        return op3('lw', f'SET_GPR_S32(ctx, {rt}, (int32_t)READ32({A}));', base, lo(rt))
    if op == 36:
        return op3('lbu', f'SET_GPR_U32(ctx, {rt}, (uint8_t)READ8({A}));', base, lo(rt))
    if op == 37:
        return op3('lhu', f'SET_GPR_U32(ctx, {rt}, (uint16_t)READ16({A}));', base, lo(rt))
    if op == 39:
        return op3('lwu', f'SET_GPR_U32(ctx, {rt}, READ32({A}));', base, lo(rt))
    if op == 55:
        return op3('ld', f'SET_GPR_U64(ctx, {rt}, READ64({A}));', base, lo(rt))
    if op == 30:
        return op3('lq', f'SET_GPR_VEC(ctx, {rt}, READ128({A}));', base, lo(rt) | hi(rt))
    if op == 40:
        return op3('sb', f'WRITE8({A}, (uint8_t){G32(rt)});', base | lo(rt), set())
    if op == 41:
        return op3('sh', f'WRITE16({A}, (uint16_t){G32(rt)});', base | lo(rt), set())
    if op == 43:
        return op3('sw', f'WRITE32({A}, {G32(rt)});', base | lo(rt), set())
    if op == 63:
        return op3('sd', f'WRITE64({A}, {G64(rt)});', base | lo(rt), set())
    if op == 31:
        return op3('sq', f'WRITE128({A}, {GV(rt)});', base | lo(rt) | hi(rt), set())
    if op == 49:
        return op3('lwc1', f'{{ uint32_t bits = READ32({A}); float f; std::memcpy(&f, &bits, sizeof(f)); ctx->f[{rt}] = f; }}', base, {f'f{rt}'})
    if op == 57:
        return op3('swc1', f'{{ float f = ctx->f[{rt}]; uint32_t bits; std::memcpy(&bits, &f, sizeof(bits)); WRITE32({A}, bits); }}', base | {f'f{rt}'}, set())
    if op == 34:
        return op3('lwl', f'{{ uint32_t addr = {A}; uint32_t aligned_addr = addr & ~3u; uint32_t offset = addr & 3u; uint32_t mem = READ32(aligned_addr); uint32_t shift = (3u - offset) << 3; uint32_t keepMask = (shift == 0) ? 0u : ((1u << shift) - 1u); uint32_t merged = ({G32(rt)} & keepMask) | (mem << shift); SET_GPR_S32(ctx, {rt}, (int32_t)merged); }}',
                   base | lo(rt), lo(rt))
    if op == 38:
        return op3('lwr', f'{{ uint32_t addr = {A}; uint32_t aligned_addr = addr & ~3u; uint32_t offset = addr & 3u; uint32_t mem = READ32(aligned_addr); uint32_t shift = offset << 3; uint32_t keepMask = (offset == 0) ? 0u : (0xFFFFFFFFu << ((4u - offset) << 3)); uint32_t merged32 = ({G32(rt)} & keepMask) | (mem >> shift); uint64_t merged64 = ({G64(rt)} & 0xFFFFFFFF00000000ull) | (uint64_t)merged32; if (offset == 0) merged64 = (uint64_t)(int64_t)(int32_t)merged32; SET_GPR_U64(ctx, {rt}, merged64); }}',
                   base | lo(rt), lo(rt))
    if op == 26:
        return op3('ldl', f'{{ uint32_t addr = {A}; uint32_t aligned_addr = addr & ~7u; uint32_t offset = addr & 7u; uint64_t mem = READ64(aligned_addr); uint32_t shift = (7u - offset) << 3; uint64_t keepMask = (shift == 0) ? 0ull : ((1ull << shift) - 1ull); SET_GPR_U64(ctx, {rt}, ({G64(rt)} & keepMask) | (mem << shift)); }}',
                   base | lo(rt), lo(rt))
    if op == 27:
        return op3('ldr', f'{{ uint32_t addr = {A}; uint32_t aligned_addr = addr & ~7u; uint32_t offset = addr & 7u; uint64_t mem = READ64(aligned_addr); uint32_t shift = offset << 3; uint64_t keepMask = (offset == 0) ? 0ull : (0xFFFFFFFFFFFFFFFFull << ((8u - offset) << 3)); SET_GPR_U64(ctx, {rt}, ({G64(rt)} & keepMask) | (mem >> shift)); }}',
                   base | lo(rt), lo(rt))
    if op == 42:
        return op3('swl', f'{{ uint32_t addr = {A}; uint32_t aligned_addr = addr & ~3u; uint32_t offset = addr & 3u; uint32_t shift = (3u - offset) << 3; uint32_t mask = 0xFFFFFFFFu >> shift; uint32_t old_data = READ32(aligned_addr); uint32_t val = {G32(rt)}; uint32_t new_data = (old_data & ~mask) | ((val >> shift) & mask); WRITE32(aligned_addr, new_data); }}',
                   base | lo(rt), set())
    if op == 46:
        return op3('swr', f'{{ uint32_t addr = {A}; uint32_t aligned_addr = addr & ~3u; uint32_t offset = addr & 3u; uint32_t shift = offset << 3; uint32_t mask = 0xFFFFFFFFu << shift; uint32_t old_data = READ32(aligned_addr); uint32_t val = {G32(rt)}; uint32_t new_data = (old_data & ~mask) | ((val << shift) & mask); WRITE32(aligned_addr, new_data); }}',
                   base | lo(rt), set())
    if op == 44:
        return op3('sdl', f'{{ uint32_t addr = {A}; uint32_t aligned_addr = addr & ~7u; uint32_t offset = addr & 7u; uint32_t shift = (7u - offset) << 3; uint64_t mask = 0xFFFFFFFFFFFFFFFFull >> shift; uint64_t old_data = READ64(aligned_addr); uint64_t val = {G64(rt)}; uint64_t new_data = (old_data & ~mask) | ((val >> shift) & mask); WRITE64(aligned_addr, new_data); }}',
                   base | lo(rt), set())
    if op == 45:
        return op3('sdr', f'{{ uint32_t addr = {A}; uint32_t aligned_addr = addr & ~7u; uint32_t offset = addr & 7u; uint32_t shift = offset << 3; uint64_t mask = 0xFFFFFFFFFFFFFFFFull << shift; uint64_t old_data = READ64(aligned_addr); uint64_t val = {G64(rt)}; uint64_t new_data = (old_data & ~mask) | ((val << shift) & mask); WRITE64(aligned_addr, new_data); }}',
                   base | lo(rt), set())
    if op == 47 or op == 51:
        i.mem = False
        i.name, i.kind = 'cache' if op == 47 else 'pref', 'nop'
        return i
    raise Unsupported(f'op {op}')


# What an instruction the translator does not emit may touch, for the
# whole-program dataflow: more than it really reads and writes, never less.
# VU0 state is not a location here; it lives in the guest context for good.
CONSERVATIVE_READS = {'hi', 'lo', 'hi1', 'lo1', 'sa', 'fcr31'}
CONSERVATIVE_WRITES = {'hi', 'lo', 'hi1', 'lo1', 'sa'}


def decode_any(addr: int, word: int) -> Insn:
    """decode(), or for an unsupported instruction its control-flow kind and
    an over-approximation of its effects (Insn.name 'unsupported')."""
    try:
        return decode(addr, word)
    except Unsupported as e:
        op = word >> 26
        rs = (word >> 21) & 31
        rt = (word >> 16) & 31
        rd = (word >> 11) & 31
        i = Insn(addr, word, name='unsupported', rs=rs, rt=rt, rd=rd)
        i.code = str(e)
        if op == 18 and rs == 8:  # bc2f/bc2t(l): a VU0 branch
            i.kind = 'branch'
            i.likely = bool(rt & 2)
            imm = word & 0xFFFF
            i.target = u32(addr + 4 + (s16(imm) << 2))
        elif op == 16 and (word & 0x3F) == 0x18 and rs == 16:  # eret
            i.kind = 'jr'
            i.rs = 31
        elif op == 1 and rt in (16, 17, 18, 19):  # bltzal & co.
            i.kind = 'unsupported-link'
        if op == 18:  # COP2: VU0 state stays in the guest context
            if rs == 1:
                i.writes |= lo(rt) | hi(rt)  # qmfc2
            elif rs == 2:
                i.writes |= lo(rt)  # cfc2
            elif rs == 5:
                i.reads |= lo(rt) | hi(rt)  # qmtc2
            elif rs == 6:
                i.reads |= lo(rt)  # ctc2
            return i
        if op in (54, 62):  # lqc2 / sqc2
            i.reads |= lo(rs)
            return i
        if op == 16:  # COP0
            if rs == 0:
                i.writes |= lo(rt)  # mfc0
            elif rs == 4:
                i.reads |= lo(rt)  # mtc0
            return i
        if op == 28:  # MMI: rd from rs and rt, 128 bits, and the multiply unit
            i.reads |= lo(rs) | hi(rs) | lo(rt) | hi(rt) | lo(rd) | hi(rd) | CONSERVATIVE_READS
            i.writes |= lo(rd) | hi(rd) | CONSERVATIVE_WRITES
            return i
        i.reads |= lo(rs) | hi(rs) | lo(rt) | hi(rt) | lo(rd) | hi(rd) | CONSERVATIVE_READS
        i.reads |= {f'f{n}' for n in range(32)} if op == 17 else set()
        i.writes |= lo(rt) | hi(rt) | lo(rd) | hi(rd) | CONSERVATIVE_WRITES
        i.writes |= {f'f{n}' for n in range(32)} | {'fcr31', 'facc'} if op == 17 else set()
        return i
