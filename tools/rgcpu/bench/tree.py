"""tree.py SRC_DIR ROOT_HEX: names of the native functions reachable from ROOT."""
import re, sys, glob
src, root = sys.argv[1], int(sys.argv[2], 16)
body, name = {}, {}
for p in glob.glob(f'{src}/nc_*.cpp'):
    cur = None
    for line in open(p):
        m = re.match(r'NcO_([0-9a-f]+) nc_\1\(.*\{$', line)
        if m:
            cur = int(m.group(1), 16); body[cur] = set(); continue
        m = re.match(r'void (\w+)_nc\(uint8_t\* rdram, R5900Context\* gctx, PS2Runtime\* runtime\) \{', line)
        if m:
            name[int(m.group(1).rsplit('_0x', 1)[1], 16)] = m.group(1); cur = None; continue
        if cur is not None:
            body[cur] |= {int(x, 16) for x in re.findall(r'\bnc_([0-9a-f]+)\(', line)}
seen, work = set(), [root]
while work:
    a = work.pop()
    if a in seen or a not in body: continue
    seen.add(a); work += body[a]
for a in sorted(seen):
    if a in name: print(name[a])
