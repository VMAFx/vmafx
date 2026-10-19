#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Find POSIX-only headers that a Windows build of a source would include.

MSVC ships no `<unistd.h>`, `<dlfcn.h>`, `<poll.h>`, `<sys/socket.h>` and
friends. A C, C++ or CUDA source that includes one compiles on every Linux
and macOS lane and fails only on the required Windows MSVC lanes, which the
local gates and the merge train never build (2026-10-08:
`core/src/cuda/import_vulkan.c` included `<unistd.h>` for `dup()` and
`close()`; the CUDA backend is built on Windows).

A finding is an include of such a header that is

- outside every preprocessor conditional that names a platform (`_WIN32`,
  `_MSC_VER`, `__linux__`, `__unix__`, `__APPLE__`, `__MINGW32__`, ...), and
- in a file the Windows build compiles: a source some `meson.build` names
  outside a block gated to another operating system
  (`host_machine.system() == 'linux'`, `!= 'windows'`, the `else` of
  `== 'windows'`, also through the `subdir()` that reads that file), or a
  header such a source includes. A source no `meson.build` names counts as
  built.

`<pthread.h>` is not listed: the Windows builds put
`core/src/compat/win32/pthread.h` on the include path. `<fcntl.h>`,
`<sys/stat.h>` and `<sys/types.h>` exist in the MSVC runtime.

Prints `path:line: <header>` for each finding. Exit status is 0 after a scan,
with or without findings (the caller decides what a finding means), and
non-zero when git cannot list the build files: no result is never a pass.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from functools import cache
from pathlib import Path

POSIX_ONLY = re.compile(
    r"^\s*#\s*include\s*<("
    r"(?:unistd|dlfcn|poll|termios|libgen|strings|dirent|glob|semaphore|sched|syslog|pwd|grp)\.h"
    r"|sys/(?:mman|ioctl|time|resource|wait|socket|un|select|uio|utsname|syscall|sysinfo"
    r"|epoll|eventfd|inotify|prctl|file|times|statvfs)\.h"
    r"|(?:linux|arpa|netinet)/[A-Za-z0-9_/-]+\.h"
    r")>"
)
CONDITIONAL = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)")
PLATFORM = re.compile(
    r"\b(_WIN32|_WIN64|_MSC_VER|__linux__|__linux|__unix__|__unix|__APPLE__|__ANDROID__"
    r"|__FreeBSD__|__MINGW32__|__MINGW64__|__CYGWIN__|_POSIX_VERSION|__has_include"
    # A feature macro the build defines when it found a POSIX dependency (GBM,
    # VA-API, dma-bufs), not a backend switch: HAVE_CUDA / HAVE_SYCL / ... are
    # defined on Windows too and guard nothing here.
    r"|\w*HAVE_(?!CUDA\b|SYCL\b|HIP\b|METAL\b|NVTX\b|DNN\b|MCP\b)\w+)\b"
)
LOCAL_INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"')
MESON_BLOCK = re.compile(r"^\s*(if|elif|else|endif)\b(.*)")
MESON_SUBDIR = re.compile(r"^\s*subdir\(\s*'([^']+)'")
MESON_FOREACH = re.compile(r"^\s*(foreach\b[^:]*:\s*(\w+)?.*|endforeach\b)")
OTHER_OS = re.compile(
    r"host_machine\.system\(\)\s*(?:==\s*'(?:linux|darwin|freebsd|openbsd|netbsd)'"
    r"|!=\s*'windows')"
)
WINDOWS_ONLY = re.compile(r"host_machine\.system\(\)\s*==\s*'windows'")
MAX_INCLUDE_DEPTH = 3  # a header counts through includers up to this many levels
MAX_DIR_DEPTH = 64  # bound of the walk from a directory up to the build root
HEADER_SUFFIXES = (".h", ".hpp", ".cuh")
# `v = host_machine.system() != 'linux' ? [] : [ ... ]`: the list is empty off Linux.
TERNARY_OFF_WINDOWS = re.compile(
    r"host_machine\.system\(\)\s*(?:!=\s*'(?:linux|darwin)'\s*\?\s*\[\s*\]"
    r"|==\s*'windows'\s*\?\s*\[\s*\])"
)


def unguarded_includes(path: Path) -> list[tuple[int, str]]:
    """(line, header) of POSIX-only includes outside a platform conditional."""
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []
    stack: list[bool] = []
    found = []
    for number, line in enumerate(lines, 1):
        cond = CONDITIONAL.match(line)
        if cond:
            keyword, rest = cond.groups()
            if keyword in ("if", "ifdef", "ifndef"):
                stack.append(bool(PLATFORM.search(rest)))
            elif keyword == "elif" and stack:
                stack[-1] = stack[-1] or bool(PLATFORM.search(rest))
            elif keyword == "endif" and stack:
                stack.pop()
            continue
        header = POSIX_ONLY.match(line)
        if header and not any(stack):
            found.append((number, header.group(1)))
    return found


def excludes_windows(condition: str, negated: bool) -> bool:
    """Whether one meson `if` condition keeps its block off Windows."""
    if negated:
        return bool(WINDOWS_ONLY.search(condition)) and not OTHER_OS.search(condition)
    return bool(OTHER_OS.search(condition))


def gated_lines(meson: Path) -> list[tuple[str, bool, tuple[str, ...]]]:
    """Per line: the text, whether an `if` keeps it off Windows, the foreach iterables around it."""
    out: list[tuple[str, bool, tuple[str, ...]]] = []
    stack: list[bool] = []
    conditions: list[str] = []
    loops: list[str] = []
    ternary_depth = 0  # bracket depth inside a list kept empty off Linux
    for raw in meson.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.split("#", 1)[0]
        in_ternary = ternary_depth > 0 or bool(TERNARY_OFF_WINDOWS.search(line))
        if in_ternary:
            ternary_depth = max(0, ternary_depth + line.count("[") - line.count("]"))
        loop = MESON_FOREACH.match(line)
        if loop:
            if loop.group(1).startswith("endforeach"):
                if loops:
                    loops.pop()
            else:
                loops.append(loop.group(2) or "")
        block = MESON_BLOCK.match(line)
        if block:
            keyword, rest = block.groups()
            if keyword == "if":
                conditions.append(rest)
                stack.append(excludes_windows(rest, False))
            elif keyword == "elif" and stack:
                # An elif is reached only when the earlier condition was false.
                stack[-1] = excludes_windows(rest, False) or excludes_windows(conditions[-1], True)
                conditions[-1] = rest
            elif keyword == "else" and stack:
                stack[-1] = excludes_windows(conditions[-1], True)
            elif keyword == "endif" and stack:
                stack.pop()
                conditions.pop()
        out.append((raw, any(stack) or in_ternary, tuple(loops)))
    return out


@cache
def meson_blocks(meson: Path) -> tuple[tuple[str, bool], ...]:
    """Per line of `meson`: whether the line sits in a block kept off Windows.

    A line inside `foreach x : v` counts as off Windows too when every
    assignment that puts an element into `v` is off Windows (`v = []` outside
    a gate and `v += [...]` only inside one: the list is empty on Windows)."""
    lines = gated_lines(meson)
    filled: dict[str, bool] = {}
    for raw, off_windows, _ in lines:
        assign = re.match(r"^\s*(\w+)\s*(\+?=)\s*(.*)", raw.split("#", 1)[0])
        if not assign:
            continue
        name, operator, value = assign.groups()
        empty = operator == "=" and value.strip() == "[]"
        if not empty:
            filled[name] = filled.get(name, True) and off_windows
    return tuple(
        (raw, off_windows or any(filled.get(v, False) for v in loops if v))
        for raw, off_windows, loops in lines
    )


def git_lines(*args: str, no_match_ok: bool = False) -> list[str]:
    """Output words of a git command; exits non-zero when git cannot run."""
    git = shutil.which("git")
    if git is None:
        raise SystemExit("find-posix-only-headers: git not found on PATH")
    done = subprocess.run(  # noqa: S603 -- resolved git, fixed argv
        [git, *args], capture_output=True, text=True, check=False
    )
    # `git grep` exits 1 when nothing matches; anything else is a failure.
    if done.returncode != 0 and not (no_match_ok and done.returncode == 1 and not done.stderr):
        raise SystemExit(f"find-posix-only-headers: git {args[0]} failed: {done.stderr.strip()}")
    return done.stdout.split()


@cache
def meson_files() -> tuple[Path, ...]:
    # Untracked files too: a new meson.build or header is part of the change.
    listed = git_lines("ls-files", "--cached", "--others", "--exclude-standard", "*meson.build")
    return tuple(Path(p) for p in listed)


def subdir_off_windows(directory: Path) -> bool:
    """Whether the parent's `subdir()` that reads directory/meson.build is gated off Windows."""
    for raw, off_windows in meson_blocks(directory.parent / "meson.build"):
        sub = MESON_SUBDIR.match(raw.split("#", 1)[0])
        if sub and (directory.parent / sub.group(1)).resolve() == directory.resolve():
            return off_windows
    return False


@cache
def meson_dir_off_windows(directory: Path) -> bool:
    """Whether a `subdir()` on the way from the build root to `directory` is gated off Windows."""
    current = directory
    for _ in range(MAX_DIR_DEPTH):
        if current == current.parent or not (current.parent / "meson.build").is_file():
            return False
        if subdir_off_windows(current):
            return True
        current = current.parent
    return False


QUOTED = re.compile(r"'([^']*)'")


def names(code: str, relative: str, stem: str) -> bool:
    """Whether a meson line's code names the file: a string that is its path
    relative to the meson.build or a tail of it (`cuda_dir + 'import_frame.c'`),
    or its stem as a target name (`executable('test_x', ...)`)."""
    for quoted in QUOTED.findall(code):
        if quoted in (stem, relative) or relative.endswith("/" + quoted):
            return True
    return False


def windows_builds_source(path: Path) -> bool:
    """A source is built on Windows unless every meson.build line naming it is gated off."""
    named = False
    resolved = path.resolve()
    for meson in meson_files():
        root = meson.parent.resolve()
        if root not in resolved.parents:
            continue
        relative = resolved.relative_to(root).as_posix()
        for raw, off_windows in meson_blocks(meson):
            if not names(raw.split("#", 1)[0], relative, path.stem):
                continue
            named = True
            if not off_windows and not meson_dir_off_windows(meson.parent):
                return True
    return not named


@cache
def includers(header_name: str) -> tuple[Path, ...]:
    listed = git_lines(
        "grep",
        "--untracked",
        "-l",
        "-E",
        rf'#\s*include\s*"([^"]*/)?{re.escape(header_name)}"',
        no_match_ok=True,
    )
    return tuple(Path(p) for p in listed)


def windows_builds(path: Path) -> bool:
    """Whether the Windows build compiles `path`: a source directly, a header
    through the sources that include it (up to MAX_INCLUDE_DEPTH levels; a
    header past that, or included by nothing, counts as a source)."""
    frontier = [path]
    seen = {path}
    for depth in range(MAX_INCLUDE_DEPTH + 1):
        following: list[Path] = []
        for item in frontier:
            users: list[Path] = []
            if item.suffix in HEADER_SUFFIXES and depth < MAX_INCLUDE_DEPTH:
                users = [user for user in includers(item.name) if user != item]
            if not users and windows_builds_source(item):
                return True
            following += [user for user in users if user not in seen]
            seen.update(users)
        frontier = following
    return False


def main(argv: list[str]) -> int:
    for name in argv:
        path = Path(name)
        found = unguarded_includes(path)
        if found and windows_builds(path):
            for number, header in found:
                print(f"{name}:{number}: <{header}>")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
