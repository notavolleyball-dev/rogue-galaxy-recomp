"""What the pinned PS2Recomp runtime can bind a guest function to.

Reads `ps2xRuntime/include/ps2_call_list.h` (PS2_SYSCALL_LIST, PS2_STUB_LIST)
and the stub bodies under `ps2xRuntime/src/lib/Kernel/Stubs/`. A stub whose
body is only `TODO_NAMED(...)` throws when called, so binding a guest function
to it is worse than recompiling the guest code.

Name resolution mirrors ps2_runtime_calls.h: exact match, else the name with
one leading underscore added or removed.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

CALL_LIST = Path("ps2xRuntime/include/ps2_call_list.h")
STUBS_DIR = Path("ps2xRuntime/src/lib/Kernel/Stubs")

_SIG = re.compile(
    r"\bvoid\s+(\w+)\s*\(\s*uint8_t\s*\*\s*rdram\s*,\s*R5900Context\s*\*\s*ctx\s*,\s*PS2Runtime\s*\*\s*runtime\s*\)\s*\{"
)


def _macro(src: str, name: str) -> list[str]:
    m = re.search(rf"#define {name}\(X\)(.*?)(?=\n\s*\n|\n#|\Z)", src, re.S)
    if not m:
        raise ValueError(f"{name} not found in {CALL_LIST}")
    return re.findall(r"X\((\w+)\)", re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S))


def _bodies(stubs_dir: Path) -> dict[str, str]:
    out = {}
    for path in sorted(stubs_dir.glob("*.cpp")):
        text = path.read_text(encoding="utf-8")
        for m in _SIG.finditer(text):
            i, depth = m.end(), 1
            while depth:
                depth += {"{": 1, "}": -1}.get(text[i], 0)
                i += 1
            out[m.group(1)] = text[m.end() : i - 1]
    return out


def _is_todo(body: str) -> bool:
    body = re.sub(r"//[^\n]*", "", body)
    body = re.sub(r"\(void\)\s*\w+\s*;", "", body).strip()
    return re.fullmatch(r"TODO(?:_NAMED)?\([^;]*\);", body) is not None


@dataclass(frozen=True)
class RuntimeCalls:
    syscalls: tuple[str, ...]
    stubs: tuple[str, ...]
    todo: frozenset[str]

    @staticmethod
    def load(fork: Path) -> "RuntimeCalls":
        src = (fork / CALL_LIST).read_text(encoding="utf-8")
        syscalls, stubs = _macro(src, "PS2_SYSCALL_LIST"), _macro(src, "PS2_STUB_LIST")
        bodies = _bodies(fork / STUBS_DIR)
        todo = frozenset(s for s in stubs if s in bodies and _is_todo(bodies[s]))
        return RuntimeCalls(tuple(syscalls), tuple(stubs), todo)

    @staticmethod
    def _resolve(name: str, entries: tuple[str, ...]) -> str | None:
        if name in entries:
            return name
        if name.startswith("_") and name[1:] in entries:
            return name[1:]
        if "_" + name in entries:
            return "_" + name
        return None

    def resolve(self, name: str) -> tuple[str, str] | None:
        """('syscall'|'stub', handler) as ps2_recomp would emit it, else None."""
        if (h := self._resolve(name, self.syscalls)) is not None:
            return "syscall", h
        if (h := self._resolve(name, self.stubs)) is not None:
            return "stub", h
        return None

    def binds_by_name(self, name: str) -> bool:
        """ps2_recomp turns a function with this name into a stub without being asked."""
        return self._resolve(name, self.stubs) is not None
