#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Add the CUDA and HIP kernel translation units to compile_commands.json.

meson compiles `.cu` files with nvcc and `.hip` files with hipcc through
CUSTOM_COMMAND rules, so they never appear in compile_commands.json and the
`cuda` / `hip` clang-tidy lanes (ADR-1142) cannot measure them. This script
reads those rules from build.ninja and appends one clang++ entry per kernel
file, keeping the rule's include paths, defines and language standard.

The entry carries no `-x cuda` / `-x hip`: the lane supplies the language and
its host-only analysis through `TIDY_RATCHET_EXTRA_<lane>` in the Makefile,
exactly as it does for the lane's host `.c` files. The `.cu` entries add
`--cuda-path` so clang finds the CUDA headers nvcc would have used.

Usage: gen-gpu-compile-commands.py <build-dir>

Existing entries are kept. An entry for a kernel file that is already present
is replaced, so rerunning after a reconfigure picks up changed flags.
"""

from __future__ import annotations

import json
import re
import shlex
import sys
from pathlib import Path

RULE_RE = re.compile(
    r"^build\s+\S+:\s+CUSTOM_COMMAND\s+(?P<src>\S+\.(?:cu|hip))\s+\|\s*(?P<tool>\S+)\s*\n"
    r"(?:[ \t]+\S[^\n]*\n)*?"
    r"[ \t]+COMMAND\s*=\s*(?P<cmd>[^\n]+)",
    re.MULTILINE,
)
KEPT_WITH_VALUE = {"-I", "-D", "-isystem", "--std", "-std"}


def kept_flags(argv: list[str]) -> list[str]:
    """The include, define and standard flags of an nvcc / hipcc command."""
    kept: list[str] = []
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg in KEPT_WITH_VALUE and i + 1 < len(argv):
            value = argv[i + 1]
            kept.append(f"-std={value}" if arg in ("--std", "-std") else arg + value)
            i += 2
            continue
        if arg.startswith(("-I", "-D", "-std=")) and arg not in KEPT_WITH_VALUE:
            kept.append(arg)
        i += 1
    return kept


def cuda_path(tool: str) -> str | None:
    """The CUDA toolkit root of an nvcc path (`<root>/bin/nvcc`)."""
    path = Path(tool)
    if path.name == "nvcc" and path.parent.name == "bin":
        return str(path.parent.parent)
    return None


def kernel_entries(build_ninja: Path) -> list[dict[str, str]]:
    build_dir = build_ninja.resolve().parent
    entries = []
    for match in RULE_RE.finditer(build_ninja.read_text(encoding="utf-8")):
        src = (build_dir / match.group("src")).resolve()
        argv = ["clang++"]
        root = cuda_path(match.group("tool"))
        if src.suffix == ".cu" and root:
            argv.append(f"--cuda-path={root}")
        argv += kept_flags(shlex.split(match.group("cmd")))
        argv += ["-c", str(src)]
        entries.append({"directory": str(build_dir), "command": shlex.join(argv), "file": str(src)})
    return entries


def main(argv: list[str]) -> int:
    if len(argv) != 2:  # noqa: PLR2004 -- exactly one argument
        print(f"usage: {argv[0]} <build-dir>", file=sys.stderr)
        return 1
    build_dir = Path(argv[1])
    ninja_path = build_dir / "build.ninja"
    compdb_path = build_dir / "compile_commands.json"
    for path in (ninja_path, compdb_path):
        if not path.is_file():
            print(f"error: {path}: no such file", file=sys.stderr)
            return 1

    added = kernel_entries(ninja_path)
    replaced = {entry["file"] for entry in added}
    existing = json.loads(compdb_path.read_text(encoding="utf-8"))
    merged = [entry for entry in existing if entry.get("file") not in replaced] + added
    compdb_path.write_text(json.dumps(merged, indent=2) + "\n", encoding="utf-8")
    print(
        f"gen-gpu-compile-commands: {len(added)} CUDA/HIP kernel entries in {compdb_path}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
