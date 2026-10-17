#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Find min() and max() calls that `<windows.h>` turns into macro invocations.

`<windows.h>` defines `min` and `max` as function-like macros unless
`NOMINMAX` is defined before it. A C++ translation unit that has read the
header and then writes `std::min(a, b)`, `std::max(a, b)` or
`std::numeric_limits<T>::max()` fails under MSVC with C2589 ("illegal token
on right side of '::'"). Every Linux and macOS lane compiles the same line,
and the Windows builds reach the header through
`core/src/compat/win32/pthread.h`, which stands in for `<pthread.h>` there
(2026-10-11: `core/src/fex_ctx_vector_internal.h` called `std::min()` in a
translation unit that had included the feature extractor headers; every MSVC
build of `master` failed).

The scan reads each C++ translation unit in the order a Windows preprocessor
would: it follows `#include "..."` into the tree, resolves `<pthread.h>` and
the other shims of `core/src/compat/win32/` to the tree's own files, skips
regions a conditional keeps off Windows (`#ifndef _WIN32`, `#ifdef __linux__`,
`#if 0`, the `#else` of a `_WIN32` branch), and switches the two macros on at
`#include <windows.h>` unless `#define NOMINMAX` came first. `#undef min` and
`#undef max` switch one off again.

A finding is a call spelled `::min(`, `.min(`, `->min(` or the `max` forms on
a line read while that macro is on, in a translation unit the Windows build
compiles. `(std::min)(a, b)` and `std::min<T>(a, b)` are no macro invocations
and are not reported.

The paths on the command line (or on standard input, with `-` as the only
argument) are the files of the change: a translation unit is reported only if
the scan of it read one of them. That covers a new call in a header and a new
include that puts an old call behind the Windows header.

Not followed: system and SDK headers (the SYCL headers also include
`<windows.h>`; `preflight.sh` has its own probe for the SYCL sources), CUDA
device code, macro bodies, and a `NOMINMAX` passed on the compiler command
line.

Prints `path:line: <call> (reached from <translation unit>)` for each finding.
Exit status is 0 after a scan, with or without findings (the caller decides
what a finding means), and non-zero when the scan cannot run or hits one of
its bounds: no result is never a pass.
"""

from __future__ import annotations

import importlib.util
import os
import re
import sys
from dataclasses import dataclass, field
from functools import cache
from pathlib import Path
from types import ModuleType

CONDITIONAL = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)")
DIRECTIVE = re.compile(r"^\s*#\s*(include|define|undef)\b\s*(.*)")
INCLUDE_TARGET = re.compile(r'^([<"])([^>"]+)[>"]')
DEFINED_TERM = re.compile(r"^(!?)\s*defined\s*\(?\s*(\w+)\s*\)?$")
USE = re.compile(r"(?:::|\.|->)\s*(min|max)\s*\(")
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT = re.compile(r"//[^\n]*")
WINDOWS_MACROS = frozenset({"_WIN32", "_WIN64", "_MSC_VER"})
OTHER_OS_MACROS = frozenset(
    {"__linux__", "__linux", "__unix__", "__unix", "__APPLE__", "__ANDROID__", "__FreeBSD__"}
)
UNIT_PATTERNS = ("*.cpp", "*.cc", "*.cxx")
# The include directories of core/src/meson.build (vmaf_base_include) and of
# the tools, then the shims the Windows builds put on the include path
# (core/meson.build: src/compat/win32, tools/compat/win32).
ANGLE_ROOTS = ("core/src/compat/win32", "core/tools/compat/win32", "core/include")
QUOTE_ROOTS = (
    "core/src",
    "core/src/feature",
    "core/src/feature/common",
    "core/tools",
    "core",
    *ANGLE_ROOTS,
)
MAX_UNITS = 4096  # translation units of one scan
MAX_STEPS = 4_000_000  # lines read for one translation unit
MAX_DEPTH = 128  # nested includes


@dataclass
class Frame:
    """One file being read: its lines, the next line, its open conditionals."""

    path: str
    lines: tuple[str, ...]
    index: int = 0
    # Per open conditional: [the current branch is off Windows, a branch Windows takes was seen].
    conditions: list[list[bool]] = field(default_factory=list)


@dataclass
class Scan:
    """What the preprocessor knows while it reads one translation unit."""

    seen: set[str]
    active: set[str] = field(default_factory=set)
    nominmax: bool = False
    uses: list[tuple[str, int, str]] = field(default_factory=list)


def fail(message: str) -> SystemExit:
    return SystemExit(f"find-windows-minmax: {message}")


def posix_scanner() -> ModuleType:
    """The sibling scanner: it knows which sources the Windows build compiles."""
    sibling = Path(__file__).with_name("find-posix-only-headers.py")
    spec = importlib.util.spec_from_file_location("find_posix_only_headers", sibling)
    if spec is None or spec.loader is None:
        raise fail(f"cannot load {sibling}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@cache
def source_lines(path: str) -> tuple[str, ...]:
    """The lines of a file with comments blanked; line numbers are kept."""
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        raise fail(f"cannot read {path}: {error}") from error
    text = BLOCK_COMMENT.sub(lambda match: "\n" * match.group().count("\n"), text)
    return tuple(LINE_COMMENT.sub("", text).splitlines())


def term_state(term: str) -> str:
    """'win', 'off' or 'unknown' for one `defined(X)` term of a condition."""
    term = term.strip()
    if term == "0":
        return "off"
    match = DEFINED_TERM.match(term)
    if not match:
        return "unknown"
    negated, name = bool(match.group(1)), match.group(2)
    if name in WINDOWS_MACROS:
        return "off" if negated else "win"
    if name in OTHER_OS_MACROS and not negated:
        return "off"
    return "unknown"


def condition_state(expression: str) -> str:
    """Whether Windows takes a branch ('win'), cannot take it ('off'), or may ('unknown')."""
    if "&&" in expression and "||" in expression:
        return "unknown"
    if "||" in expression:
        states = [term_state(term) for term in expression.split("||")]
        if "win" in states:
            return "win"
        return "off" if all(state == "off" for state in states) else "unknown"
    states = [term_state(term) for term in expression.split("&&")]
    if "off" in states:
        return "off"
    return "win" if all(state == "win" for state in states) else "unknown"


def step_conditional(conditions: list[list[bool]], keyword: str, rest: str) -> None:
    """Apply one #if / #elif / #else / #endif to a file's open conditionals."""
    if keyword in ("if", "ifdef", "ifndef"):
        expression = {"ifdef": f"defined({rest.strip()})", "ifndef": f"!defined({rest.strip()})"}
        state = condition_state(expression.get(keyword, rest))
        conditions.append([state == "off", state == "win"])
    elif not conditions:
        return
    elif keyword == "endif":
        conditions.pop()
    elif keyword == "else":
        conditions[-1][0] = conditions[-1][1]
    else:  # elif: reached only when no earlier branch was taken
        state = condition_state(rest)
        conditions[-1][0] = conditions[-1][1] or state == "off"
        conditions[-1][1] = conditions[-1][1] or state == "win"


@cache
def resolve(includer: str, bracket: str, name: str) -> str | None:
    """The tree's file an include names on Windows, or None for a system header."""
    bases = ANGLE_ROOTS if bracket == "<" else (str(Path(includer).parent), *QUOTE_ROOTS)
    for base in bases:
        candidate = os.path.normpath(Path(base) / name)
        if Path(candidate).is_file():
            return candidate
    return None


def read_directive(scan: Scan, path: str, keyword: str, rest: str) -> str | None:
    """Apply an #include, #define or #undef; return a file to read next, if any."""
    if keyword == "define":
        scan.nominmax = scan.nominmax or rest.split()[:1] == ["NOMINMAX"]
        return None
    if keyword == "undef":
        scan.active.discard(rest.strip())
        return None
    include = INCLUDE_TARGET.match(rest)
    if not include:
        return None
    bracket, name = include.groups()
    if name.lower() == "windows.h":
        if not scan.nominmax:
            scan.active.update(("min", "max"))
        return None
    target = resolve(path, bracket, name)
    if target is None or target in scan.seen:
        return None
    scan.seen.add(target)
    return target


def read_line(scan: Scan, frame: Frame, line: str) -> str | None:
    """Read one line of a file; return a file to read next, if the line includes one."""
    conditional = CONDITIONAL.match(line)
    if conditional:
        step_conditional(frame.conditions, *conditional.groups())
        return None
    if any(off for off, _ in frame.conditions):
        return None
    directive = DIRECTIVE.match(line)
    if directive:
        return read_directive(scan, frame.path, *directive.groups())
    use = USE.search(line)
    if use and use.group(1) in scan.active:
        scan.uses.append((frame.path, frame.index, use.group(0)))
    return None


def scan_unit(unit: str) -> Scan:
    """Read one translation unit as a Windows preprocessor would."""
    scan = Scan(seen={unit})
    frames = [Frame(unit, source_lines(unit))]
    for _ in range(MAX_STEPS):
        if not frames:
            return scan
        frame = frames[-1]
        if frame.index >= len(frame.lines):
            frames.pop()
            continue
        frame.index += 1
        included = read_line(scan, frame, frame.lines[frame.index - 1])
        if included is None:
            continue
        if len(frames) >= MAX_DEPTH:
            raise fail(f"{unit}: more than {MAX_DEPTH} nested includes")
        frames.append(Frame(included, source_lines(included)))
    raise fail(f"{unit}: more than {MAX_STEPS} lines read")


def changed_paths(argv: list[str]) -> set[str]:
    names = sys.stdin.read().split() if argv == ["-"] else argv
    return {os.path.normpath(name) for name in names}


def main(argv: list[str]) -> int:
    changed = changed_paths(argv)
    if not changed:
        return 0
    posix = posix_scanner()
    listed = posix.git_lines(
        "ls-files", "--cached", "--others", "--exclude-standard", *UNIT_PATTERNS
    )
    units = sorted({os.path.normpath(name) for name in listed})
    if len(units) > MAX_UNITS:
        raise fail(f"more than {MAX_UNITS} translation units")
    reported: set[tuple[str, int]] = set()
    for unit in units:
        if not Path(unit).is_file():  # deleted in the working tree
            continue
        scan = scan_unit(unit)
        if not scan.uses or changed.isdisjoint(scan.seen):
            continue
        if not posix.windows_builds_source(Path(unit)):
            continue
        for path, line, call in scan.uses:
            if (path, line) not in reported:
                reported.add((path, line))
                print(f"{path}:{line}: {call} behind <windows.h> (reached from {unit})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
