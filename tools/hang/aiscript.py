"""Decode an AI script VM's bytecode from a dump of the guest RAM.

usage: python tools/hang/aiscript.py RDRAM.bin VM_ADDR [desc_off ...]
  VM_ADDR: a script VM (hex); the AI ones hold 0x3E8 at +4 and 0x4DBB10 at +8
  (the AI command table). With no desc_off the VM's current function is
  decoded, the current command marked >>; otherwise each function whose
  descriptor is at code+desc_off (the operand of a CALLSUB). ALL=1 goes on
  past the first RETURN. Command names come from the handler list at
  0x4511E0 through tools/mipsdis.py, cached in build/run/aicmds.json.
"""
import json, os, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
ram = open(sys.argv[1], 'rb').read()
vm = int(sys.argv[2], 16)


def u32(a): return struct.unpack_from('<I', ram, a & 0x1FFFFFFF)[0]
def s32(a): return struct.unpack_from('<i', ram, a & 0x1FFFFFFF)[0]
def f32(a): return struct.unpack_from('<f', ram, a & 0x1FFFFFFF)[0]


def cmd_names():
    cache = os.path.join(ROOT, 'build', 'run', 'aicmds.json')
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    if os.path.exists(cache):
        return {int(k): v for k, v in json.load(open(cache)).items()}
    elf = open(os.path.join(ROOT, 'disc', 'SCUS_974.90'), 'rb').read()
    pairs = []
    a = 0x4511E0
    while True:
        h, i = struct.unpack_from('<Ii', elf, 0x180 + a - 0x100000)
        if h == 0:
            break
        pairs.append((i, h))
        a += 8
    out = subprocess.run(['python', os.path.join(ROOT, 'tools', 'mipsdis.py'), os.path.join(ROOT, 'disc', 'SCUS_974.90'),
                          os.path.join(ROOT, 'config', 'SCUS_974.90', 'functions_main.csv'), 'sym'] +
                         ['%x' % h for _, h in pairs], capture_output=True, text=True, cwd=ROOT).stdout
    nm = {}
    for line in out.splitlines():
        m = re.match(r'([0-9a-fA-F]+):\s+(\S+)', line)
        if m:
            nm[int(m.group(1), 16)] = m.group(2)
    names = {i: nm.get(h, '%08x' % h) for i, h in pairs}
    json.dump(names, open(cache, 'w'))
    return names


NAMES = cmd_names()
ALL = os.environ.get('ALL') == '1'
code = u32(vm + 0x48)
CMP = {0: '==', 1: '!=', 2: '<', 3: '<=', 4: '>', 5: '>='}


def fmt(v):
    if v[0] == 'c':
        return ('%g' % v[1]) if isinstance(v[1], float) else str(v[1])
    if v[0] == 'var':
        return '&%s%d' % ('L' if v[2] == 1 else 'k%d_' % v[2], v[1])
    if v[0] == 'ref':
        return '%s%d' % ('L' if v[2] == 1 else 'k%d_' % v[2], v[1])
    return v[0]


def decode(start, marks=(), limit=4000):
    st = []
    a = start
    depth = 0
    while a < start + limit * 12:
        op, x, y = u32(a), s32(a + 4), s32(a + 8)
        off = a - code
        note = ''
        if op == 3:
            v = struct.unpack('<f', struct.pack('<i', y))[0] if x == 2 else y
            st.append(('c', v))
            note = 'push ' + fmt(st[-1])
        elif op == 2:
            st.append(('var', x, y)); note = 'push ' + fmt(st[-1])
        elif op == 1:
            st.append(('ref', x, y)); note = 'push ' + fmt(st[-1])
        elif op in (6, 7, 8, 9, 10, 12, 13):
            b = st.pop() if st else ('?',)
            c = st.pop() if st else ('?',)
            sym = {6: '+', 7: '*?', 8: '-', 9: '*', 10: '/', 12: '&&?', 13: '||?'}[op]
            if op == 6 and b[0] == 'c' and c[0] == 'c':
                st.append(('c', c[1] + b[1]))
            else:
                st.append(('expr', '(%s %s %s)' % (fmt(c), sym, fmt(b))))
            note = '%s -> %s' % (sym, fmt(st[-1]) if st[-1][0] != 'expr' else st[-1][1])
        elif op == 11:
            b = st.pop() if st else ('?',)
            st.append(('c', -b[1]) if b[0] == 'c' else ('expr', '-' + fmt(b)))
            note = 'neg'
        elif op == 5:
            v = st.pop() if st else ('?',)
            r = st.pop() if st else ('?',)
            st.append(v)
            note = '%s = %s' % (fmt(r)[1:] if r[0] == 'var' else fmt(r), fmt(v) if v[0] != 'expr' else v[1])
        elif op == 4:
            if st: st.pop()
            note = 'pop'
        elif op == 21:
            args = st[-x:] if x > 0 else []
            st = st[:-x] if x > 0 else st
            st.append(('ret', 'ret'))
            fn = args[0][1] if args and args[0][0] == 'c' else None
            note = 'CALL %s(%s)' % ((NAMES.get(fn, 'fn%d' % fn) + '[%d]' % fn) if fn is not None else '?',
                                   ', '.join(fmt(v) if v[0] != 'expr' else v[1] for v in args[1:]))
        elif op == 14:
            b = st.pop() if st else ('?',)
            c = st.pop() if st else ('?',)
            st.append(('cmp', 'cmp'))
            note = 'CMP %s %s %s' % (fmt(c) if c[0] != 'expr' else c[1], CMP.get(x - 0x28, x), fmt(b) if b[0] != 'expr' else b[1])
        elif op == 16:
            note = 'JMP +%x' % x
        elif op == 17:
            if st: st.pop()
            note = 'JMP-IF-FALSE +%x%s' % (x, ' (keep)' if y else '')
        elif op == 18:
            if st: st.pop()
            note = 'JMP-IF-TRUE +%x%s' % (x, ' (keep)' if y else '')
        elif op == 19:
            d = code + y
            nargs = u32(d + 12)
            args = st[-nargs:] if nargs > 0 else []
            st = st[:-nargs] if nargs > 0 else st
            st.append(('ret', 'sub'))
            note = 'CALLSUB desc+%x entry+%x (%s)' % (y, u32(d), ', '.join(fmt(v) if v[0] != 'expr' else v[1] for v in args))
        elif op == 20:
            st = st[:-x]; note = 'POP %d' % x
        elif op == 22:
            note = 'label %d' % x
        elif op == 23:
            note = 'YIELD'
        elif op == 15:
            v = st.pop() if st else ('?',)
            note = 'RETURN %s' % (fmt(v) if v[0] != 'expr' else v[1])
        elif op == 27:
            note = 'END'
        elif op == 28:
            note = 'SKIP-END'
        else:
            note = 'op%d %d %d' % (op, x, y)
        print('%s+%05x: %s%s' % ('>>' if a in marks else '  ', off, '  ' * depth, note))
        if (op == 15 or op == 27) and not ALL:
            break
        a += 12


if len(sys.argv) > 3:
    for d in sys.argv[3:]:
        off = int(d, 16)
        desc = code + off
        print('=== sub desc+%x: entry +%x slots %d args %d' % (off, u32(desc), u32(desc + 8), u32(desc + 12)))
        decode(code + u32(desc))
else:
    func = u32(vm + 0x34)
    print('=== current function desc %08x entry +%x' % (func, u32(func)))
    decode(code + u32(func), marks=(u32(vm + 0x38),))
