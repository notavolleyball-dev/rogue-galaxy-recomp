"""Small R5900 (MIPS) disassembler for SCUS_974.90 with symbol lookup.

usage: mipsdis.py ELF CSV dis START END          disassemble [START, END)
       mipsdis.py ELF CSV refs ADDR [ADDR...]     find lui/addiu|lw|lwc1 pairs and gp-rel loads of ADDR
       mipsdis.py ELF CSV find HEX                find byte pattern (hex string, as in memory) in the image
       mipsdis.py ELF CSV calls ADDR              find jal/j to ADDR
       mipsdis.py ELF CSV sym ADDR                symbol containing ADDR
"""
import csv
import struct
import sys

BASE = 0x100000
FILEOFF = 0x180
GP = None  # set from entry

REGS = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]


def load(elf, csvpath):
    data = open(elf, "rb").read()
    syms = []
    with open(csvpath, newline="") as f:
        r = csv.reader(f)
        next(r)
        for name, start, end, size in r:
            syms.append((int(start, 16), int(end, 16), name))
    syms.sort()
    return data, syms


def symat(syms, addr):
    lo, hi = 0, len(syms)
    while lo < hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            lo = mid + 1
        else:
            hi = mid
    i = lo - 1
    if i >= 0 and syms[i][0] <= addr < syms[i][1]:
        s = syms[i]
        off = addr - s[0]
        return s[2] + (f"+0x{off:x}" if off else "")
    return None


def word(data, addr):
    off = addr - BASE + FILEOFF
    if off < 0 or off + 4 > len(data):
        return None
    return struct.unpack_from("<I", data, off)[0]


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


def fmt_mem(off, base):
    return f"{sx16(off):#x}({REGS[base]})" if sx16(off) < 0 else f"{off:#x}({REGS[base]})"


def dis(data, syms, addr, w):
    op = w >> 26
    rs = (w >> 21) & 31
    rt = (w >> 16) & 31
    rd = (w >> 11) & 31
    sa = (w >> 6) & 31
    fn = w & 63
    imm = w & 0xFFFF
    simm = sx16(imm)
    target = ((addr + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
    br = addr + 4 + simm * 4
    R = REGS

    def brs(a):
        s = symat(syms, a)
        return f"{a:#x}" + (f" <{s}>" if s else "")

    if op == 0:
        names = {0: "sll", 2: "srl", 3: "sra", 4: "sllv", 6: "srlv", 7: "srav", 8: "jr", 9: "jalr", 10: "movz",
                 11: "movn", 12: "syscall", 13: "break", 15: "sync", 16: "mfhi", 17: "mthi", 18: "mflo", 19: "mtlo",
                 20: "dsllv", 22: "dsrlv", 23: "dsrav", 24: "mult", 25: "multu", 26: "div", 27: "divu",
                 32: "add", 33: "addu", 34: "sub", 35: "subu", 36: "and", 37: "or", 38: "xor", 39: "nor",
                 40: "mfsa", 41: "mtsa", 42: "slt", 43: "sltu", 44: "dadd", 45: "daddu", 46: "dsub", 47: "dsubu",
                 48: "tge", 49: "tgeu", 50: "tlt", 51: "tltu", 52: "teq", 54: "tne", 56: "dsll", 58: "dsrl",
                 59: "dsra", 60: "dsll32", 62: "dsrl32", 63: "dsra32"}
        n = names.get(fn, f"special_{fn}")
        if w == 0:
            return "nop"
        if fn in (0, 2, 3, 56, 58, 59, 60, 62, 63):
            return f"{n} {R[rd]},{R[rt]},{sa}"
        if fn in (4, 6, 7, 20, 22, 23):
            return f"{n} {R[rd]},{R[rt]},{R[rs]}"
        if fn == 8:
            return f"jr {R[rs]}"
        if fn == 9:
            return f"jalr {R[rd]},{R[rs]}"
        if fn in (12, 13, 15):
            return n
        if fn in (16, 18, 40):
            return f"{n} {R[rd]}"
        if fn in (17, 19, 41):
            return f"{n} {R[rs]}"
        if fn in (24, 25, 26, 27):
            return f"{n} {R[rd]},{R[rs]},{R[rt]}" if rd else f"{n} {R[rs]},{R[rt]}"
        return f"{n} {R[rd]},{R[rs]},{R[rt]}"
    if op == 1:
        names = {0: "bltz", 1: "bgez", 2: "bltzl", 3: "bgezl", 16: "bltzal", 17: "bgezal", 18: "bltzall",
                 19: "bgezall", 24: "mtsab", 25: "mtsah"}
        return f"{names.get(rt, f'regimm_{rt}')} {R[rs]},{brs(br)}"
    if op == 2:
        return f"j {brs(target)}"
    if op == 3:
        return f"jal {brs(target)}"
    if op in (4, 5, 20, 21):
        n = {4: "beq", 5: "bne", 20: "beql", 21: "bnel"}[op]
        if rt == 0 and op == 4 and rs == 0:
            return f"b {brs(br)}"
        return f"{n} {R[rs]},{R[rt]},{brs(br)}"
    if op in (6, 7, 22, 23):
        n = {6: "blez", 7: "bgtz", 22: "blezl", 23: "bgtzl"}[op]
        return f"{n} {R[rs]},{brs(br)}"
    if op in (8, 9, 24, 25):
        n = {8: "addi", 9: "addiu", 24: "daddi", 25: "daddiu"}[op]
        return f"{n} {R[rt]},{R[rs]},{simm:#x}"
    if op in (10, 11):
        n = {10: "slti", 11: "sltiu"}[op]
        return f"{n} {R[rt]},{R[rs]},{simm:#x}"
    if op in (12, 13, 14):
        n = {12: "andi", 13: "ori", 14: "xori"}[op]
        return f"{n} {R[rt]},{R[rs]},{imm:#x}"
    if op == 15:
        return f"lui {R[rt]},{imm:#x}"
    if op == 16:
        if rs == 0:
            return f"mfc0 {R[rt]},${rd}"
        if rs == 4:
            return f"mtc0 {R[rt]},${rd}"
        return f"cop0 {w:#x}"
    if op == 17:
        fs, ft, fd = rd, rt, sa
        if rs == 0:
            return f"mfc1 {R[rt]},$f{fs}"
        if rs == 2:
            return f"cfc1 {R[rt]},$f{fs}"
        if rs == 4:
            return f"mtc1 {R[rt]},$f{fs}"
        if rs == 6:
            return f"ctc1 {R[rt]},$f{fs}"
        if rs == 8:
            n = {0: "bc1f", 1: "bc1t", 2: "bc1fl", 3: "bc1tl"}.get(rt & 3)
            return f"{n} {brs(br)}"
        if rs == 16:
            names = {0: "add.s", 1: "sub.s", 2: "mul.s", 3: "div.s", 4: "sqrt.s", 5: "abs.s", 6: "mov.s", 7: "neg.s",
                     22: "rsqrt.s", 24: "adda.s", 25: "suba.s", 26: "mula.s", 28: "madd.s", 29: "msub.s",
                     30: "madda.s", 31: "msuba.s", 36: "cvt.w.s", 40: "max.s", 41: "min.s", 48: "c.f.s",
                     50: "c.eq.s", 52: "c.lt.s", 54: "c.le.s"}
            n = names.get(fn, f"cop1s_{fn}")
            if fn in (0, 1, 2, 3, 40, 41, 28, 29):
                return f"{n} $f{fd},$f{fs},$f{ft}"
            if fn in (24, 25, 26, 30, 31, 48, 50, 52, 54):
                return f"{n} $f{fs},$f{ft}"
            return f"{n} $f{fd},$f{fs}"
        if rs == 20:
            if fn == 32:
                return f"cvt.s.w $f{fd},$f{fs}"
            return f"cop1w {w:#x}"
        return f"cop1 {w:#x}"
    if op == 18:
        return f"cop2 {w:#x}"
    if op == 28:
        names = {0: "madd", 1: "maddu", 4: "plzcw", 8: "mmi0", 9: "mmi2", 16: "mfhi1", 17: "mthi1", 18: "mflo1",
                 19: "mtlo1", 24: "mult1", 25: "multu1", 26: "div1", 27: "divu1", 32: "madd1", 33: "maddu1",
                 40: "mmi1", 41: "mmi3", 48: "pmfhl", 49: "pmthl", 52: "psllh", 54: "psrlh", 55: "psrah",
                 60: "psllw", 62: "psrlw", 63: "psraw"}
        n = names.get(fn, f"mmi_{fn}")
        if fn in (24, 25, 26, 27, 0, 1, 32, 33):
            return f"{n} {R[rd]},{R[rs]},{R[rt]}" if rd else f"{n} {R[rs]},{R[rt]}"
        if fn in (16, 18):
            return f"{n} {R[rd]}"
        return f"{n} {w:#x}"
    if op == 30:
        return f"lq {R[rt]},{fmt_mem(imm, rs)}"
    if op == 31:
        return f"sq {R[rt]},{fmt_mem(imm, rs)}"
    mem = {32: "lb", 33: "lh", 34: "lwl", 35: "lw", 36: "lbu", 37: "lhu", 38: "lwr", 39: "lwu", 40: "sb", 41: "sh",
           42: "swl", 43: "sw", 44: "sdl", 45: "sdr", 46: "swr", 47: "cache", 26: "ldl", 27: "ldr", 55: "ld",
           63: "sd", 51: "pref"}
    if op in mem:
        return f"{mem[op]} {R[rt]},{fmt_mem(imm, rs)}"
    if op == 49:
        return f"lwc1 $f{rt},{fmt_mem(imm, rs)}"
    if op == 57:
        return f"swc1 $f{rt},{fmt_mem(imm, rs)}"
    if op == 54:
        return f"lqc2 vf{rt},{fmt_mem(imm, rs)}"
    if op == 62:
        return f"sqc2 vf{rt},{fmt_mem(imm, rs)}"
    return f".word {w:#010x}"


def find_gp(data):
    # entry: lui gp,X ; addiu gp,gp,Y  (anywhere in the first 0x300 bytes)
    hi = lo = None
    for a in range(BASE, BASE + 0x300, 4):
        w = word(data, a)
        if (w >> 26) == 15 and ((w >> 16) & 31) == 28:
            hi = (w & 0xFFFF) << 16
        if (w >> 26) == 9 and ((w >> 16) & 31) == 28 and ((w >> 21) & 31) == 28 and hi is not None:
            lo = sx16(w & 0xFFFF)
            return (hi + lo) & 0xFFFFFFFF
    return None


def disrange(data, syms, start, end, out=sys.stdout):
    for a in range(start, end, 4):
        w = word(data, a)
        if w is None:
            break
        s = symat(syms, a)
        if s and not s.split("+")[-1].startswith("0x") or (s and "+0x" not in s):
            out.write(f"\n{s}:\n")
        extra = ""
        if GP is not None and (w >> 26) in (35, 36, 37, 43, 49, 57, 32, 33, 40, 41, 39, 55, 63) and ((w >> 21) & 31) == 28:
            extra = f"    ; gp{sx16(w & 0xFFFF):+#x} = {(GP + sx16(w & 0xFFFF)) & 0xFFFFFFFF:#x}"
        if (w >> 26) == 9 and ((w >> 21) & 31) == 28 and GP is not None:
            extra = f"    ; = {(GP + sx16(w & 0xFFFF)) & 0xFFFFFFFF:#x}"
        out.write(f"{a:08x}  {w:08x}  {dis(data, syms, a, w)}{extra}\n")


def refs(data, syms, targets, text_end):
    """lui rX,hi then within 16 insns an addiu/lw/sw/lwc1/swc1/... rY, lo(rX); and gp-relative."""
    hits = []
    tset = set(targets)
    for a in range(BASE, text_end, 4):
        w = word(data, a)
        op = w >> 26
        if op == 15:
            rt = (w >> 16) & 31
            hi = (w & 0xFFFF) << 16
            for b in range(a + 4, min(a + 4 + 24 * 4, text_end), 4):
                w2 = word(data, b)
                op2 = w2 >> 26
                if op2 in (9, 13, 35, 36, 37, 43, 49, 57, 32, 33, 40, 41, 39, 55, 63, 30, 31, 54, 62) and ((w2 >> 21) & 31) == rt:
                    lo = (w2 & 0xFFFF) if op2 == 13 else sx16(w2 & 0xFFFF)
                    ea = (hi + lo) & 0xFFFFFFFF
                    if ea in tset:
                        hits.append((b, ea, f"{dis(data, syms, a, w)} ; {dis(data, syms, b, w2)}"))
                # a redefinition of rt ends the chain (approx: any write to rt by addiu/lui/lw)
                if op2 in (15, 9, 35) and ((w2 >> 16) & 31) == rt and not (op2 == 9 and ((w2 >> 21) & 31) == rt):
                    break
        if GP is not None and op in (35, 36, 37, 43, 49, 57, 32, 33, 40, 41, 39, 55, 63, 9) and ((w >> 21) & 31) == 28:
            ea = (GP + sx16(w & 0xFFFF)) & 0xFFFFFFFF
            if ea in tset:
                hits.append((a, ea, dis(data, syms, a, w)))
    for a, ea, txt in sorted(set(hits)):
        print(f"{a:08x} -> {ea:08x}  [{symat(syms, a)}]  {txt}")


def main():
    global GP
    elf, csvp, cmd = sys.argv[1], sys.argv[2], sys.argv[3]
    data, syms = load(elf, csvp)
    GP = find_gp(data) or 0x485170
    text_end = syms[-1][1]
    if cmd == "gp":
        print(f"gp = {GP:#x}")
    elif cmd == "dis":
        disrange(data, syms, int(sys.argv[4], 16), int(sys.argv[5], 16))
    elif cmd == "refs":
        refs(data, syms, [int(x, 16) for x in sys.argv[4:]], text_end)
    elif cmd == "find":
        pat = bytes.fromhex(sys.argv[4])
        i = 0
        while True:
            i = data.find(pat, i)
            if i < 0:
                break
            print(f"{i - FILEOFF + BASE:08x}  (file {i:#x})")
            i += 1
    elif cmd == "calls":
        tg = int(sys.argv[4], 16)
        for a in range(BASE, text_end, 4):
            w = word(data, a)
            if (w >> 26) in (2, 3):
                target = ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if target == tg:
                    print(f"{a:08x}  [{symat(syms, a)}]  {dis(data, syms, a, w)}")
    elif cmd == "scan":
        pass
    elif cmd == "sym":
        for x in sys.argv[4:]:
            print(f"{x}: {symat(syms, int(x, 16))}")


if __name__ == "__main__":
    main()


def scan(data, syms, value, mask, start, end, ctx_after=0):
    for a in range(start, end, 4):
        w = word(data, a)
        if w is None:
            break
        if (w & mask) == value:
            print(f"{a:08x}  {w:08x}  [{symat(syms, a)}]  {dis(data, syms, a, w)}")
            for b in range(a + 4, a + 4 + ctx_after * 4, 4):
                w2 = word(data, b)
                print(f"    {b:08x}  {w2:08x}  {dis(data, syms, b, w2)}")


if __name__ == "__main__" and sys.argv[3] == "scan":
    _d, _s = load(sys.argv[1], sys.argv[2])
    GP = 0x485170
    _v, _m = int(sys.argv[4], 16), int(sys.argv[5], 16)
    _st = int(sys.argv[6], 16) if len(sys.argv) > 6 else BASE
    _en = int(sys.argv[7], 16) if len(sys.argv) > 7 else _s[-1][1]
    _ca = int(sys.argv[8]) if len(sys.argv) > 8 else 0
    scan(_d, _s, _v, _m, _st, _en, _ca)
