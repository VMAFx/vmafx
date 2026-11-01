#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fork Python under python/ and testdata/ passes the process and file-mode rules.

pyproject.toml exempts python/** and testdata/** from ruff's S family for the
upstream Netflix harness. That exemption also hid fork-written files there, and
Codacy's Bandit then flagged them on the pull request (B603, B607, B103). This
check runs exactly those three rules on the given files whose licence header names
the fork licence, EUPL-1.2, without the per-file exemption.
Run a process through scripts/lib/safe_subprocess.run instead.

Ruff's S103 only reports group or other bits, but the Semgrep rule Codacy runs
(python.lang.security.audit.insecure-file-permissions) compares the number: it reports
any os.chmod, os.lchmod or os.fchmod mode from 0o650 up (0o700 included), a mode that
names a group or other stat flag, and `| 0o111`. `mode_findings` applies the same
thresholds; give an executable test fixture 0o500 and a data file 0o600 or 0o644.

Usage: check_fork_python_subprocess.py FILE...; exit 1 lists each finding.
"""

from __future__ import annotations

import ast
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from scripts.lib.safe_subprocess import run  # noqa: E402 - needs the repository root

RULES = "S103,S603,S607"
SCOPES = ("python/", "testdata/")
HEADER_LINES = 5
FORK_SPDX = "SPDX-License-" + "Identifier: EUPL-1.2"  # split: REUSE reads the literal
CHMOD_CALLS = frozenset({"chmod", "lchmod", "fchmod"})
WIDE_STAT_FLAGS = frozenset({"S_IWGRP", "S_IXGRP", "S_IWOTH", "S_IXOTH", "S_IRWXO", "S_IRWXG"})
ALL_EXECUTE = 0o111
# Semgrep's bounds: plain modes from 0o650, and st_mode values (file type bits) from 0o100650.
WIDE_MODE = 0o650
FILE_TYPE_BITS = 0o100000
WIDE_ST_MODE = 0o100650
MODE_ARG = 1


def _wide_mode(mode: ast.expr) -> bool:
    """True for a mode the Semgrep insecure-file-permissions rule reports."""
    if isinstance(mode, ast.Constant) and isinstance(mode.value, int):
        bits = mode.value
        return WIDE_MODE <= bits < FILE_TYPE_BITS or bits >= WIDE_ST_MODE
    if isinstance(mode, ast.BinOp) and isinstance(mode.op, ast.BitOr):
        right = mode.right
        if isinstance(right, ast.Constant) and right.value == ALL_EXECUTE:
            return True
    names = {n.attr for n in ast.walk(mode) if isinstance(n, ast.Attribute)}
    names |= {n.id for n in ast.walk(mode) if isinstance(n, ast.Name)}
    return bool(names & WIDE_STAT_FLAGS)


def mode_findings(path: str) -> list[str]:
    """'path:line: mode' for each os.chmod-family call with a mode Semgrep reports."""
    tree = ast.parse(Path(path).read_text(encoding="utf-8"), filename=path)
    found: list[str] = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call) or len(node.args) <= MODE_ARG:
            continue
        func = node.func
        if not (isinstance(func, ast.Attribute) and isinstance(func.value, ast.Name)):
            continue
        if func.value.id == "os" and func.attr in CHMOD_CALLS and _wide_mode(node.args[MODE_ARG]):
            mode = ast.unparse(node.args[MODE_ARG])
            found.append(f"{path}:{node.lineno}: os.{func.attr} mode {mode}")
    return found


def fork_files(paths: list[str]) -> list[str]:
    """The paths under the exempt scopes whose header names the fork licence."""
    chosen: list[str] = []
    for name in paths:
        rel = Path(name).as_posix()
        if not (rel.endswith(".py") and rel.startswith(SCOPES)) or not Path(name).is_file():
            continue
        with Path(name).open(encoding="utf-8", errors="replace") as handle:
            head = [handle.readline() for _ in range(HEADER_LINES)]
        if any(FORK_SPDX in line for line in head):
            chosen.append(name)
    return chosen


def main(argv: list[str]) -> int:
    files = fork_files(argv)
    if not files:
        return 0
    modes = [line for name in files for line in mode_findings(name)]
    for line in modes:
        print(
            f"check_fork_python_subprocess: {line} reaches 0o650 or a group/other bit",
            file=sys.stderr,
        )
    ruff = shutil.which("ruff")
    if ruff is None:
        print("check_fork_python_subprocess: ruff is not on PATH", file=sys.stderr)
        return 2
    result = run(
        [
            ruff,
            "check",
            "--isolated",
            "--no-cache",
            "--select",
            RULES,
            "--output-format",
            "concise",
            *files,
        ],
        allowed_executables=[ruff],
        capture_output=True,
        text=True,
        timeout_seconds=120,
    )
    if result.returncode == 0 and not modes:
        return 0
    sys.stderr.write(result.stdout + result.stderr)
    print(
        "check_fork_python_subprocess: run processes through scripts/lib/safe_subprocess.run "
        "and give files a mode below 0o650 (0o500 for an executable fixture)",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
