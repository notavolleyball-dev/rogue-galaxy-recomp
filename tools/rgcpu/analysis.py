"""Whole-program dataflow for the native CPU translator.

Every function of the image (translated or not) gets:
  MAYDEF  locations its call tree may write
  PRES    locations it provably gives back unchanged (s0-s7, gp, sp, fp, ra,
          their upper halves, f20-f31) through the usual save/restore
  IN      locations its call tree may read before writing (its parameters)
  OUT     MAYDEF that some caller reads afterwards, minus PRES (its results)

Calls the translator cannot follow (jalr, syscalls, host stubs, addresses
that are not a known function) are runtime boundaries and follow the MIPS
ABI: they read ABI_IN, may write ABI_CLOBBER, give back the callee-saved
registers. A function the old world enters (the function table: jalr,
threads, unconverted callers) must deliver ABI_LIVE.
"""
from __future__ import annotations

from r5900 import Insn

SREGS = [16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31]
CANDIDATES = {f'r{n}' for n in SREGS} | {f'h{n}' for n in SREGS} | {f'f{n}' for n in range(20, 32)}
ALL = ({f'r{n}' for n in range(1, 32)} | {f'h{n}' for n in range(1, 32)} | {f'f{n}' for n in range(32)}
       | {'fcr31', 'hi', 'lo', 'hi1', 'lo1', 'sa', 'facc'})
ABI_IN = ({f'r{n}' for n in range(4, 12)} | {f'r{n}' for n in SREGS} | {f'h{n}' for n in SREGS}
          | {f'f{n}' for n in range(12, 32)} | {'fcr31'})
SYSCALL_IN = ABI_IN | {'r2', 'r3'}
ABI_CLOBBER = (ALL - CANDIDATES) | {'r31', 'h31'}
ABI_LIVE = {'r2', 'r3', 'f0', 'f1'} | CANDIDATES

# The calling convention as the contract between translated functions
# (RGCPU_EXACT=1 turns it off, for diffs against the oracle that compare every
# register). A caller relies on a callee for its results and for the
# callee-saved registers, nothing else: what else a callee leaves in a
# temporary is garbage to compiled code. Without it every temporary a callee
# might set and might leave alone had to be passed in and handed back, all
# the way up the call graph (85-parameter signatures in the hot paths).
import os as _os
EXACT = _os.environ.get('RGCPU_EXACT') == '1'
ABI_RESULTS = {'r2', 'r3', 'h2', 'h3', 'f0', 'f1'} | CANDIDATES
# Callee-saved registers a conforming callee only saves and restores, never
# reads for their value (gp, sp and ra it does read). A boundary needs them
# neither in the guest context before nor reloaded after: the native caller's
# locals already hold them. Taking them as read made every function with a
# syscall or jalr below it take s0-s7, fp, their upper halves and f20-f31.
BOUNDARY_SAVED = set() if EXACT else CANDIDATES - {'r28', 'r29', 'r31'}
BOUNDARY_IN = ABI_IN - BOUNDARY_SAVED
BOUNDARY_SYSCALL_IN = SYSCALL_IN - BOUNDARY_SAVED

EXIT = 'exit'
STORE_WIDTH = {'sb': 1, 'sh': 2, 'sw': 4, 'swl': 4, 'swr': 4, 'swc1': 4, 'sd': 8, 'sdl': 8, 'sdr': 8, 'sq': 16}
SLOT_SIZE = {'q': 16, 'd': 8, 'f': 4}


class FA:
    """One function's control flow, as nodes the dataflow walks.

    Nodes are instruction indices, plus ('call', k) after the delay slot of
    a call at k and ('tail', k) after a jump out of the function. A branch's
    delay slot is its own node; the branch decides where it goes next.
    """

    def __init__(self, fn, insns: list[Insn], starts: dict[int, object]):
        self.fn, self.insns = fn, insns
        self.at = {ins.addr: k for k, ins in enumerate(insns)}
        self.succ: dict = {}
        self.callee: dict = {}  # ('call'|'tail', k) -> Function or None (boundary)
        self.boundary_in: dict = {}  # node -> locations a boundary reads
        self.labels: set[int] = set()
        self.problems: list[str] = []
        n = len(insns)
        for ins in insns:
            if ins.kind in ('branch', 'jump') and ins.target in self.at:
                self.labels.add(ins.target)
            for t in ins.targets or ():
                if t in self.at:
                    self.labels.add(t)
        self.everything_live: set = set()  # nodes where all locations are live
        self.pres_saved: set = set()  # BOUNDARY_SAVED it preserves (analyse)
        self.call_kill: dict = {}  # call/tail node -> locations the callee surely sets (analyse)
        everything = set(range(n))
        delay_slots = set()
        for k, ins in enumerate(insns):
            if k in delay_slots:
                continue  # its successors come from the branch before it
            nxt = {k + 1} if k + 1 < n else {EXIT}
            if ins.kind in ('op', 'nop', 'syscall', 'break', 'unsupported-link'):
                self.succ[k] = nxt
                if ins.kind in ('syscall', 'break'):
                    self.boundary_in[k] = BOUNDARY_SYSCALL_IN
                continue
            if k + 1 >= n:
                self.problems.append('branch in the last slot')
                self.succ[k] = {EXIT}
                continue
            d = k + 1
            delay_slots.add(d)
            after = {k + 2} if k + 2 < n else {EXIT}
            fall_d = after if insns[d].addr in self.labels else set()
            if insns[d].kind not in ('op', 'nop'):
                self.problems.append('branch in a delay slot')
            if ins.kind == 'branch':
                if ins.target not in self.at:
                    self.problems.append(f'{ins.name} leaves the function')
                    tgt = {EXIT}
                else:
                    tgt = {self.at[ins.target]}
                if ins.likely:
                    self.succ[k] = {d} | after
                    self.succ[d] = tgt | fall_d
                else:
                    self.succ[k] = {d}
                    self.succ[d] = tgt | after
            elif ins.kind == 'jump':
                self.succ[k] = {d}
                if ins.target in self.at:
                    self.succ[d] = {self.at[ins.target]} | fall_d
                else:
                    node = ('tail', k)
                    self.succ[d] = {node} | fall_d
                    self.succ[node] = {EXIT}
                    self.callee[node] = starts.get(ins.target)
                    if self.callee[node] is None or self.callee[node].external:
                        self.boundary_in[node] = BOUNDARY_IN
            elif ins.kind in ('call', 'jalr'):
                node = ('call', k)
                self.succ[k] = {d}
                self.succ[d] = {node} | fall_d
                self.succ[node] = after
                self.callee[node] = starts.get(ins.target) if ins.kind == 'call' else None
                if self.callee[node] is None or self.callee[node].external:
                    self.boundary_in[node] = BOUNDARY_IN
                if fall_d:
                    self.problems.append('a call\'s delay slot is a branch target')
            elif ins.kind == 'jr':
                self.succ[k] = {d}
                if ins.rs == 31:
                    self.succ[d] = {EXIT} | fall_d
                elif ins.targets:
                    # A jump table: its cases, or out of the function (the
                    # default: a tail jump through the runtime).
                    node = ('jt', k)
                    self.succ[d] = {self.at[t] for t in ins.targets if t in self.at} | {node} | fall_d
                    self.succ[node] = {EXIT}
                    self.callee[node] = None
                    self.boundary_in[node] = BOUNDARY_IN
                else:
                    # A tail jump to code we cannot see (PtmfScall's `jr $t9`,
                    # a pointer-to-member call). Its target is a compiled
                    # function entered with the calling convention, so it
                    # reads what a call through jalr reads, not everything:
                    # taking everything as live here made every caller up the
                    # call graph pass and spill all ~100 registers (3,304
                    # functions with 100+ parameters).
                    self.problems.append('jr through a register')
                    node = ('jt', k)
                    self.succ[d] = {node} | fall_d
                    self.succ[node] = {EXIT}
                    self.callee[node] = None
                    self.boundary_in[node] = BOUNDARY_IN
                    self.everything_live.add(d)
            else:
                self.problems.append(f'{ins.kind}')
                self.succ[k] = nxt
        self.nodes = list(self.succ.keys())
        self.pred: dict = {node: set() for node in self.nodes}
        self.pred[EXIT] = set()
        for node, ss in self.succ.items():
            for s in ss:
                self.pred.setdefault(s, set()).add(node)

    def uses_kills(self, node, IN):
        """Locations a node reads and the ones it certainly overwrites."""
        if node in self.boundary_in:  # calls out, and syscall instructions
            return self.boundary_in[node], set()
        if isinstance(node, tuple):
            # What the callee writes on every path back (MUSTDEF less what it
            # preserves): its old value cannot reach past the call.
            return IN[self.callee[node].start], self.call_kill.get(node, set())
        ins = self.insns[node]
        if ins.name in ('unsupported', 'vu0', 'mmi'):
            return ins.reads, set()  # effects over-approximated: kill nothing
        if BOUNDARY_SAVED and ins.rs == 29 and ins.name in ('sd', 'sq', 'swc1'):
            # Saving a callee-saved register in the prologue does not use its
            # value (BOUNDARY_SAVED): the caller keeps its own, PRES says it
            # comes back. Any other read of it still makes it an input.
            # Only for registers this function provably gives back (PRES):
            # OperatorNew (0x100990) restores s0/fp through fp, unproven, so
            # they are its outputs and the saved value must be the real one.
            reg = f'f{ins.rt}' if ins.name == 'swc1' else f'r{ins.rt}'
            if reg in self.pres_saved:
                return ins.reads - {reg, f'h{ins.rt}'}, ins.writes
        return ins.reads, ins.writes


def maydef_local(fa: FA) -> set:
    s = set()
    for ins in fa.insns:
        s |= ins.writes
    for node, callee in fa.callee.items():
        if node in fa.boundary_in:
            s |= ABI_CLOBBER
    for ins in fa.insns:
        if ins.kind == 'syscall':
            s |= ABI_CLOBBER
    # A tail jump through a register (PtmfScall, 0x100e70: a pointer-to-member
    # call) leaves for code we cannot see, which returns to our caller with
    # whatever it set: at least what a call may. Without this, v0 from the
    # member function was not an output, and the native caller tested a stale
    # v0 (SaveModeDispatch, 0x375df0: the memory card check never finished).
    if fa.everything_live:
        s |= ABI_CLOBBER
    return s


def analyse(fas: dict[int, FA]) -> dict:
    """MAYDEF, PRES, IN, OUT for every function (keyed by start address)."""
    MAYDEF = {a: maydef_local(fa) for a, fa in fas.items()}
    changed = True
    while changed:
        changed = False
        for a, fa in fas.items():
            s = MAYDEF[a]
            for node, callee in fa.callee.items():
                if callee is not None and not callee.external and callee.start in MAYDEF:
                    if not MAYDEF[callee.start] <= s:
                        s |= MAYDEF[callee.start]
                        changed = True

    # Who calls whom, to redo only what a change can reach.
    callers: dict[int, set[int]] = {a: set() for a in fas}
    for a, fa in fas.items():
        for callee in fa.callee.values():
            if callee is not None and callee.start in callers:
                callers[callee.start].add(a)

    # PRES: from "everything preserved" down; a function is redone when a
    # callee's set shrinks.
    PRES = {a: set(CANDIDATES) for a in fas}
    work = list(fas)
    queued = set(work)
    while work:
        a = work.pop()
        queued.discard(a)
        p = preserved(fas[a], PRES)
        if p != PRES[a]:
            PRES[a] = p
            for c in callers[a]:
                if c not in queued:
                    queued.add(c)
                    work.append(c)

    # MUSTDEF: what a function writes on every path to its return, from
    # "everything" down (a function that never returns keeps everything).
    # A call kills MUSTDEF less PRES; without this nothing was killed at a
    # call, so a result the caller reads after the call (f3, r1, ...) was live
    # before it, at the caller's entry, and so in every caller's IN: the
    # 40+ parameter signatures that spill around every call.
    MUST = {a: set(ALL) for a in fas}
    work = list(fas)
    queued = set(work)
    while work:
        a = work.pop()
        queued.discard(a)
        m = must_local(fas[a], MUST)
        if m != MUST[a]:
            MUST[a] = m
            for c in callers[a]:
                if c not in queued:
                    queued.add(c)
                    work.append(c)
    for a, fa in fas.items():
        fa.pres_saved = PRES[a] & BOUNDARY_SAVED
        fa.call_kill = {}
        for node, callee in fa.callee.items():
            if node in fa.boundary_in or callee is None or callee.external or callee.start not in MUST:
                continue
            fa.call_kill[node] = MUST[callee.start] - PRES[callee.start] - {'r29', 'h29'}

    # IN and OUT: up from empty. A function is redone when a callee's IN
    # grows (its call sites read more) or its own OUT grows (a caller needs
    # more back); a callee is redone when what is live after its calls grows.
    IN = {a: set() for a in fas}
    NEED = {a: set(ABI_LIVE) for a in fas}
    OUT = {a: (MAYDEF[a] & NEED[a]) - PRES[a] for a in fas}
    work = list(fas)
    queued = set(work)
    rounds = 0
    while work:
        a = work.pop()
        queued.discard(a)
        rounds += 1
        fa = fas[a]
        live_in, live_out = liveness(fa, IN, OUT[a])
        entry = live_in.get(0, set()) - {'r0', 'h0'}
        redo = set()
        if entry != IN[a]:
            IN[a] = entry
            redo |= callers[a]
        for node, callee in fa.callee.items():
            if callee is None or callee.external or callee.start not in NEED:
                continue
            need = live_out[node] if node[0] == 'call' else OUT[a]
            if not EXACT:
                need = need & ABI_RESULTS
            c = callee.start
            if not need <= NEED[c]:
                NEED[c] |= need
                new_out = (MAYDEF[c] & NEED[c]) - PRES[c]
                if new_out != OUT[c]:
                    OUT[c] = new_out
                    redo.add(c)
                    redo |= callers[c]
        for b in redo:
            if b not in queued:
                queued.add(b)
                work.append(b)
    return {'MAYDEF': MAYDEF, 'PRES': PRES, 'MUST': MUST, 'IN': IN, 'OUT': OUT, 'rounds': rounds}


def must_local(fa: FA, MUST: dict) -> set:
    """Locations written on every path from entry to the exit."""
    state = {0: frozenset()}
    work = [0]
    at_exit = None
    while work:
        node = work.pop()
        cur = state[node]
        if node in fa.boundary_in:
            out = cur
        elif isinstance(node, tuple):
            out = cur | MUST.get(fa.callee[node].start, set())
        else:
            ins = fa.insns[node]
            out = cur if ins.name in ('unsupported', 'vu0', 'mmi') else cur | ins.writes
        for s in fa.succ.get(node, ()):
            if s == EXIT:
                at_exit = out if at_exit is None else at_exit & out
                continue
            old = state.get(s)
            new = out if old is None else old & out
            if old is None or new != old:
                state[s] = frozenset(new)
                work.append(s)
    if at_exit is None:
        return set(ALL)
    return set(at_exit) - {'r0', 'h0'}


def liveness(fa: FA, IN: dict, out_f: set):
    live_in = {node: set() for node in fa.nodes}
    live_out = {node: set() for node in fa.nodes}
    exit_live = set(out_f) | {'r31'}
    work = list(fa.nodes)
    in_work = set(work)
    while work:
        node = work.pop()
        in_work.discard(node)
        o = set()
        for s in fa.succ[node]:
            o |= exit_live if s == EXIT else live_in[s]
        live_out[node] = o
        uses, kills = fa.uses_kills(node, IN)
        i = uses | (o - kills)
        if i != live_in[node]:
            live_in[node] = i
            for p in fa.pred.get(node, ()):
                if p not in in_work:
                    work.append(p)
                    in_work.add(p)
    return live_in, live_out


def preserved(fa: FA, PRES: dict) -> set:
    """Candidates whose entry value is back in place at every return.

    Forward over the nodes with: the sp offset from entry (or None), which
    candidates still hold their entry value, and which sp-relative slots hold
    which candidate's entry value. A store through any other base is taken
    not to touch the frame's save slots.
    """
    E = 'E'
    start = (0, frozenset(CANDIDATES), frozenset())
    state = {0: start}
    work = [0]
    exits = []
    insns = fa.insns

    def join(a, b):
        if a is None:
            return b
        sp = a[0] if a[0] == b[0] else None
        return (sp, a[1] & b[1], a[2] & b[2])

    while work:
        node = work.pop()
        sp, vals, slots = state[node]
        vals, slots = set(vals), set(slots)
        if isinstance(node, tuple):
            callee = fa.callee[node]
            if node in fa.boundary_in:
                keep = CANDIDATES - {'r31', 'h31'}
            else:
                keep = PRES.get(callee.start, set())
            vals &= keep | {'r29', 'h29'}  # calls give sp back (see sp_offsets)
        else:
            ins = insns[node]
            n = ins.name
            writes = ins.writes
            if n in ('addiu', 'daddiu') and ins.rs == 29 and ins.rt == 29 and sp is not None:
                imm = ins.word & 0xFFFF
                sp += imm - 0x10000 if imm & 0x8000 else imm
                writes = writes - {'r29'}  # sp is judged by its offset at the exits
            elif 'r29' in ins.writes:
                sp = None
            off = None
            if ins.mem and ins.rs == 29 and sp is not None:
                imm = ins.word & 0xFFFF
                off = sp + (imm - 0x10000 if imm & 0x8000 else imm)
            rt = ins.rt
            width = STORE_WIDTH.get(n)
            if width and off is not None:
                # A store into the frame: it replaces what overlaps it.
                slots = {sl for sl in slots if sl[0] + SLOT_SIZE[sl[1]] <= off or off + width <= sl[0]}
                if n == 'sq' and f'r{rt}' in vals and f'h{rt}' in vals:
                    slots.add((off, 'q', rt))
                elif n == 'sd' and f'r{rt}' in vals:
                    slots.add((off, 'd', rt))
                elif n == 'swc1' and f'f{rt}' in vals:
                    slots.add((off, 'f', rt))
            elif n in ('unsupported', 'vu0', 'mmi') and ins.rs == 29:
                slots = set()  # a VU0 store into the frame: forget the slots
            restored = set()
            if n in ('lq', 'ld', 'lwc1') and off is not None:
                if n == 'lq' and (off, 'q', rt) in slots:
                    restored = {f'r{rt}', f'h{rt}'}
                elif n == 'ld' and ((off, 'd', rt) in slots or (off, 'q', rt) in slots):
                    restored = {f'r{rt}'}
                elif n == 'lwc1' and (off, 'f', rt) in slots:
                    restored = {f'f{rt}'}
            vals -= writes
            vals |= restored
        new = (sp, frozenset(vals & CANDIDATES), frozenset(slots))
        for s in fa.succ[node]:
            if s == EXIT:
                exits.append(new)
                continue
            if isinstance(s, tuple) and s[0] == 'tail':
                pass
            old = state.get(s)
            j = join(old, new)
            if j != old:
                state[s] = j
                work.append(s)
    if not exits:
        return set(CANDIDATES)
    p = set(CANDIDATES)
    for sp, vals, _slots in exits:
        p &= set(vals)
        if sp != 0:
            p -= {'r29', 'h29'}
    # A tail jump's callee returns for us: it must preserve them too (the
    # tail node's own transfer above already applied its PRES).
    return p


# ---------------------------------------------------------------------------
# Frames: the PS2 stack is ours. A function's frame slots become C++ locals
# (one constant-indexed byte array the compiler turns into registers), except
# what something else can see: a slot whose address escapes, the incoming
# argument area above the frame, what a callee reads as its incoming
# arguments, and anything an instruction we cannot redirect touches.

ACCESS_WIDTH = {'lb': 1, 'lbu': 1, 'sb': 1, 'lh': 2, 'lhu': 2, 'sh': 2, 'lw': 4, 'lwu': 4, 'sw': 4,
                'lwc1': 4, 'swc1': 4, 'ld': 8, 'sd': 8, 'lq': 16, 'sq': 16,
                'lwl': 4, 'lwr': 4, 'swl': 4, 'swr': 4, 'ldl': 8, 'ldr': 8, 'sdl': 8, 'sdr': 8}
REDIRECTABLE = {'lb', 'lbu', 'sb', 'lh', 'lhu', 'sh', 'lw', 'lwu', 'sw', 'lwc1', 'swc1', 'ld', 'sd', 'lq', 'sq'}
OUTGOING_GUESS = 64  # bytes above sp an unknown callee may read as stack arguments


def s16v(word):
    imm = word & 0xFFFF
    return imm - 0x10000 if imm & 0x8000 else imm


def sp_offsets(fa: FA, PRES: dict) -> dict:
    """sp - entry sp at the start of each node, or None where unknown."""
    state = {0: 0}
    work = [0]
    while work:
        node = work.pop()
        sp = state[node]
        if isinstance(node, tuple):
            # Every call gives sp back: code after a call relies on it on the
            # PS2 as well, and a callee that never returns makes it moot.
            out = sp
        else:
            ins = fa.insns[node]
            if ins.name in ('addiu', 'daddiu') and ins.rs == 29 and ins.rt == 29 and sp is not None:
                out = sp + s16v(ins.word)
            elif 'r29' in ins.writes:
                out = None
            else:
                out = sp
        for s in fa.succ[node]:
            if s == EXIT:
                continue
            if s not in state:
                state[s] = out
                work.append(s)
            elif state[s] != out and state[s] is not None:
                state[s] = None
                work.append(s)
    return state


class Frame:
    def __init__(self):
        self.ok = False
        self.lo = 0  # lowest promoted offset (relative to entry sp)
        self.size = 0
        self.slot = {}  # node -> offset relative to entry sp, for redirected accesses
        self.incoming: set = set()  # offsets >= 0 it touches (its caller's frame)
        self.reason = ''


def frame_plans(fas: dict, A: dict) -> dict:
    PRES = A['PRES']
    plans = {}
    sps = {}
    for a, fa in fas.items():
        sps[a] = sp_offsets(fa, PRES)
        f = Frame()
        for node, sp in sps[a].items():
            if isinstance(node, tuple) or sp is None:
                continue
            ins = fa.insns[node]
            if ins.rs == 29 and (ins.mem or ins.name in ('vu0', 'unsupported')):
                off = sp + s16v(ins.word)
                if off >= 0:
                    f.incoming.add(off)
        plans[a] = f

    for a, fa in fas.items():
        f = plans[a]
        sp_at = sps[a]
        memory = set()  # bytes (offset relative to entry sp) that stay in guest memory
        guessed = set()  # bytes an unknown callee might read as stack arguments
        accesses = {}
        escaped = False
        for node, sp in sp_at.items():
            if isinstance(node, tuple):
                callee = fa.callee[node]
                if sp is None:
                    continue
                if node in fa.boundary_in or callee is None or callee.external:
                    guessed |= set(range(sp, sp + OUTGOING_GUESS))
                else:
                    for k in plans[callee.start].incoming:
                        memory |= set(range(sp + k, sp + k + 16))
                continue
            ins = fa.insns[node]
            if ins.kind == 'syscall' and sp is not None:
                guessed |= set(range(sp, sp + OUTGOING_GUESS))
            reads_sp = 'r29' in ins.reads
            if not reads_sp:
                continue
            if ins.name in ('addiu', 'daddiu') and ins.rs == 29 and ins.rt == 29:
                continue
            base_access = ins.rs == 29 and (ins.mem or ins.name in ('vu0', 'unsupported'))
            if base_access and not (ins.mem and ins.rt == 29 and ins.name not in ('sw', 'sd', 'sq', 'sb', 'sh')):
                if sp is None:
                    f.reason = 'sp unknown at an access'
                    break
                off = sp + s16v(ins.word)
                if ins.name in REDIRECTABLE and off < 0:
                    accesses[node] = (off, ACCESS_WIDTH[ins.name])
                else:
                    w = ACCESS_WIDTH.get(ins.name, 16)
                    memory |= set(range(off, off + w))
                # A store of sp itself into the frame is an escape.
                if ins.mem and ins.rt == 29 and ins.name in ('sw', 'sd', 'sq'):
                    escaped = True
                continue
            escaped = True  # sp goes somewhere other than a base or itself
        else:
            # Saved-register slots: only ever a callee-saved register stored
            # and loaded back into the same register. Never a stack argument,
            # never what an escaped pointer is for.
            # Full width both ways (sd/ld, sq/lq, swc1/lwc1), stored and
            # loaded: `sw $s7, 0($sp)` with s7 = sp+0x10 is a pointer handed
            # to a callee through the escaped frame, not a save
            # (VfiprintfR, 0x136894).
            regs_at, kinds_at = {}, {}
            for node, (off, w) in accesses.items():
                ins = fa.insns[node]
                reg = f'f{ins.rt}' if ins.name in ('lwc1', 'swc1') else f'r{ins.rt}'
                regs_at.setdefault(off, set()).add((reg, w))
                kinds_at.setdefault(off, set()).add(ins.name)
            full = ({'sd', 'ld'}, {'sq', 'lq'}, {'swc1', 'lwc1'}, {'sq', 'lq', 'ld'})
            saves = {off for off, regs in regs_at.items()
                     if len(regs) == 1 and next(iter(regs))[0] in CANDIDATES
                     and kinds_at[off] in full}
            if escaped:
                accesses = {n: v for n, v in accesses.items() if v[0] in saves}
            accesses = {n: (off, w) for n, (off, w) in accesses.items()
                        if not (set(range(off, off + w)) & memory)
                        and (off in saves or not (set(range(off, off + w)) & guessed))}
            if accesses:
                f.lo = min(off for off, _w in accesses.values())
                hi = max(off + w for off, w in accesses.values())
                f.size = (hi - f.lo + 15) & ~15
                f.slot = {n: off for n, (off, _w) in accesses.items()}
                f.ok = True
    return plans
