#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every SYCL translation unit compiles ahead of time for every default target (ADR-1468).

    test_sycl_aot_default_targets.py --build-dir <meson build dir> [--jobs N]

The default configuration compiles each SYCL translation unit with ocloc for
the 19 targets of ``sycl_icpx_aot_targets``, and one kernel a target cannot
compile fails the build. A lane build is usually configured for one device or
for none (``-Dsycl_icpx_aot_targets=``), so it never runs that compile: the
dev container image, which uses the default, did not build for a day while
every lane was green.

This test runs in a build of any configuration and needs no device:

1. It measures with ocloc which required sub-group sizes each default target
   accepts and compares that with ``sycl_aot_targets.SIZES_BY_FAMILY``, the
   table the device-free contract rests on.
2. If the build was not configured with the full default list, it compiles
   every SYCL translation unit of ``build.ninja`` again, for the full list,
   into a scratch directory, and reports each unit that fails with the
   compiler's lines for the kernel and the target.

Exit 0: everything compiles. Exit 1: a failure, listed. Exit 77 (skip): the
build has no icpx SYCL units, or ocloc is not installed; the reason is
printed. It takes minutes, so it is in the suite ``sycl-aot`` and not in
``fast``; ``test_sycl_sub_group_size_contract.py`` is the fast check.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import sycl_aot_targets as aot

SKIP = 77
# One SYCL unit compiled by icpx: its output, source and command line.
STATEMENT = re.compile(
    r"^build\s+(\S+):\s+CUSTOM_COMMAND(?:_DEP)?\s+(\S+\.cpp)\s+\|.*icpx\s*\n"
    r"(?:[ \t]+\S[^\n]*\n)*?"
    r"[ \t]+COMMAND\s*=\s*(.+?)(?:\n|$)",
    re.MULTILINE,
)
# What core/src/meson.build passes for ahead-of-time compilation (ADR-1360).
AOT_ARGS = (
    "-fsycl",
    "-fno-sycl-rdc",
    "--offload-compress",
    "-fsycl-targets=spir64_gen,spir64",
    "-Xsycl-target-backend=spir64_gen",
)
DIAGNOSTIC = re.compile(r"error: in kernel|unsupported on this platform|Build failed for")


def unescape(command: str) -> str:
    """A ninja COMMAND as a shell line: `$ ` is a space, `$$` a dollar."""
    return command.replace("$ ", " ").replace("$:", ":").replace("$$", "$")


def units(build_ninja: str) -> list[tuple[str, str, list[str]]]:
    """(output, source, argv) of every icpx-compiled SYCL unit."""
    return [
        (match.group(1), match.group(2), shlex.split(unescape(match.group(3))))
        for match in STATEMENT.finditer(build_ninja)
    ]


def configured_targets(argv: list[str]) -> list[str]:
    """The targets a unit's command compiles for; empty for a JIT-only build."""
    for argument in argv:
        if argument.startswith("-device "):
            return [name for name in argument[len("-device ") :].split(",") if name]
    return []


def for_targets(argv: list[str], targets: list[str], output: str) -> list[str]:
    """The unit's command, compiling for `targets` into `output`, no depfile."""
    device = "-device " + ",".join(targets)
    rewritten: list[str] = []
    skip = False
    for argument in argv:
        if skip:
            skip = False
        elif argument == "-fsycl":
            rewritten += [*AOT_ARGS, device]
        elif argument in AOT_ARGS or argument.startswith("-device ") or argument == "-MD":
            continue
        elif argument == "-MF":
            skip = True
        elif argument == "-o":
            rewritten += ["-o", output]
            skip = True
        else:
            rewritten.append(argument)
    return rewritten


def measured_size_failures(ocloc: str, targets: list[str], workdir: Path) -> list[str]:
    """Where ocloc and SIZES_BY_FAMILY disagree about a target's sub-group sizes."""
    failures = []
    for target in targets:
        accepted = tuple(
            size for size in aot.PROBED_SIZES if aot.ocloc_accepts(ocloc, target, size, workdir)
        )
        if accepted != aot.supported_sizes(target):
            failures.append(
                f"{target}: ocloc accepts required sub-group sizes {accepted}, "
                f"sycl_aot_targets.SIZES_BY_FAMILY says {aot.supported_sizes(target)}"
            )
    return failures


def compile_unit(build_dir: Path, source: str, argv: list[str]) -> tuple[str, list[str]]:
    """Run one rewritten compile; the unit's diagnostics when it fails."""
    result = subprocess.run(  # noqa: S603 -- the build's own compile command, no shell
        argv, cwd=build_dir, capture_output=True, text=True, timeout=3000, check=False
    )
    if result.returncode == 0:
        return source, []
    text = result.stdout + result.stderr
    lines = [line.strip() for line in text.splitlines() if DIAGNOSTIC.search(line)]
    return source, lines or [text.strip()[-2000:] or f"exit status {result.returncode}"]


def compile_failures(build_dir: Path, targets: list[str], jobs: int, workdir: Path) -> list[str]:
    """Every unit of the build that does not compile for `targets`."""
    found = units((build_dir / "build.ninja").read_text(encoding="utf-8"))
    tasks = [
        (source, for_targets(argv, targets, str(workdir / f"{index}.o")))
        for index, (_output, source, argv) in enumerate(found)
    ]
    failures = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for source, lines in pool.map(lambda task: compile_unit(build_dir, *task), tasks):
            for line in lines:
                failures.append(f"{source}: {line}")
    print(f"compiled {len(tasks)} SYCL translation units for {len(targets)} targets")
    return failures


def run(build_dir: Path, jobs: int) -> int:
    ninja = build_dir / "build.ninja"
    found = units(ninja.read_text(encoding="utf-8")) if ninja.is_file() else []
    if not found:
        print(f"skip: {ninja} compiles no SYCL translation unit with icpx")
        return SKIP
    ocloc = shutil.which("ocloc")
    if ocloc is None:
        print("skip: ocloc is not on PATH (scripts/ci/install-intel-ocloc.sh)")
        return SKIP
    targets = aot.default_targets()
    with tempfile.TemporaryDirectory(prefix="vmaf-sycl-aot-") as scratch:
        workdir = Path(scratch)
        failures = measured_size_failures(ocloc, targets, workdir)
        configured = configured_targets(found[0][2])
        if set(targets) <= set(configured):
            print(
                f"the build compiled its {len(found)} SYCL translation units for the default list"
            )
        else:
            failures += compile_failures(build_dir, targets, jobs, workdir)
    for failure in failures:
        print(f"FAIL: {failure}")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=int(os.environ.get("VMAF_SYCL_AOT_JOBS", "4")))
    arguments = parser.parse_args()
    return run(arguments.build_dir.resolve(), max(1, arguments.jobs))


if __name__ == "__main__":
    sys.exit(main())
