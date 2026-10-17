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
The `hip` lane also appends an entry for the standalone probe
``scripts/dev/hip_dispatch_drop_probe.hip``, mirroring the flags of the device kernels.
"""

from __future__ import annotations

import json
import re
import shlex
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PROBE_REL = Path("scripts/dev/hip_dispatch_drop_probe.hip")

# One ninja build statement of a meson custom target, with its variable block.
# meson names the rule CUSTOM_COMMAND_DEP when the target has a depfile.
STATEMENT_RE = re.compile(
    r"^build\s+[^:\n]+:\s+CUSTOM_COMMAND(?:_DEP)?\s+(?P<inputs>[^\n]*)\n"
    r"(?P<body>(?:[ \t]+\S[^\n]*(?:\n|$))*)",
    re.MULTILINE,
)
ANY_STATEMENT_RE = re.compile(r"^build\s+[^:\n]+:\s+\S+\s+(?P<inputs>[^\n]*)$", re.MULTILINE)
COMMAND_RE = re.compile(r"^[ \t]+COMMAND\s*=\s*(?P<cmd>[^\n]+)", re.MULTILINE)
KERNEL_SUFFIXES = (".cu", ".hip")
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


def kernel_source(inputs: str) -> str | None:
    """The kernel file among the explicit inputs of a build statement.

    The explicit inputs end at the first ``|``: what follows are the implicit
    dependencies (the compiler, and every header a target lists in
    ``depend_files``) and the order-only ones.
    """
    for token in inputs.split("|", 1)[0].split():
        if token.endswith(KERNEL_SUFFIXES):
            return token
    return None


class UnparsedKernelRuleError(ValueError):
    """A kernel build statement the generator cannot turn into an entry."""


def kernel_entries(build_ninja: Path) -> list[dict[str, str]]:
    """One clang++ entry per nvcc / hipcc build statement of *build_ninja*.

    A statement that compiles a kernel file but carries no COMMAND raises:
    a lane that silently drops its kernels still reports a clean measurement.
    """
    build_dir = build_ninja.resolve().parent
    entries = []
    for statement in STATEMENT_RE.finditer(build_ninja.read_text(encoding="utf-8")):
        source = kernel_source(statement.group("inputs"))
        if source is None:
            continue
        command = COMMAND_RE.search(statement.group("body"))
        if command is None:
            raise UnparsedKernelRuleError(f"{source}: build statement has no COMMAND")
        compiler = shlex.split(command.group("cmd"))
        src = (build_dir / source).resolve()
        argv = ["clang++"]
        root = cuda_path(compiler[0])
        if src.suffix == ".cu" and root:
            argv.append(f"--cuda-path={root}")
        argv += kept_flags(compiler)
        argv += ["-c", str(src)]
        entries.append({"directory": str(build_dir), "command": shlex.join(argv), "file": str(src)})
    return entries


def kernel_statements(build_ninja: Path) -> int:
    """Build statements of any rule whose explicit inputs name a kernel file.

    Counted without the rule name on purpose: when meson renames or reshapes
    the custom-command rule, this still sees the kernels and `main` refuses a
    database that lost them.
    """
    text = build_ninja.read_text(encoding="utf-8")
    return sum(
        kernel_source(match.group("inputs")) is not None
        for match in ANY_STATEMENT_RE.finditer(text)
    )


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

    try:
        added = kernel_entries(ninja_path)
    except UnparsedKernelRuleError as exc:
        print(f"error: {ninja_path}: {exc}", file=sys.stderr)
        return 1
    expected = kernel_statements(ninja_path)
    if len(added) != expected:
        # The parser fell out of step with meson's rule layout. Fail rather
        # than hand the lane a database without its kernels.
        print(
            f"error: {ninja_path}: {expected} build statements compile a .cu / .hip "
            f"file, {len(added)} were parsed",
            file=sys.stderr,
        )
        return 1

    hip_entries = [e for e in added if e["file"].endswith(".hip")]
    if hip_entries:
        probe_source = (REPO_ROOT / PROBE_REL).resolve()
        if not probe_source.is_file():
            print(f"error: {probe_source}: missing HIP dispatch drop probe source", file=sys.stderr)
            return 1
        first_hip_argv = shlex.split(hip_entries[0]["command"])
        hip_flags = first_hip_argv[1:-2]
        probe_entry = {
            "directory": str(build_dir),
            "command": shlex.join(["clang++", *hip_flags, "-c", str(probe_source)]),
            "file": str(probe_source),
        }
        added.append(probe_entry)

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
