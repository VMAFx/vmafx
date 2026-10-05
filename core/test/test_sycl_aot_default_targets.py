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
3. From the objects of that compile it reads every target's kernel image and
   fails on a kernel that uses scratch memory (a register spill or a private
   array, ADR-1395) on any target, outside ``sycl_aot_scratch.KNOWN_SCRATCH``.
   The device audit ``test_sycl_kernel_scratch`` sees only the GPU it runs
   on; Xe-LP, which has no 256-entry register file, spilled in kernels that
   were scratch-free on the Arc A380 and B580. A build configured with the
   full list compiled its images compressed, so the check is not run there and
   the test says so.

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
from dataclasses import dataclass, field
from pathlib import Path

import sycl_aot_scratch as scratch
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
# The images are compressed into the object only to save space; uncompressed,
# sycl_aot_scratch reads them from the object.
COMPRESSION = "--offload-compress"
MATCHED_IDS = re.compile(r"Matched ids:\s*(\d+\.\d+\.\d+)")


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
    """The unit's command, compiling for `targets` into `output`, no depfile, uncompressed."""
    device = "-device " + ",".join(targets)
    rewritten: list[str] = []
    skip = False
    for argument in argv:
        if skip:
            skip = False
        elif argument == "-fsycl":
            rewritten += [*(a for a in AOT_ARGS if a != COMPRESSION), device]
        elif argument in (*AOT_ARGS, "-MD") or argument.startswith(("-device ", COMPRESSION)):
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


def target_ips(ocloc: str, targets: list[str]) -> dict[str, set[str]]:
    """GFX IP version -> the families of the default targets that have it (`ocloc ids`)."""
    families: dict[str, set[str]] = {}
    for target in targets:
        result = subprocess.run(  # noqa: S603 -- the resolved ocloc, fixed arguments, no shell
            [ocloc, "ids", target], capture_output=True, text=True, timeout=120, check=False
        )
        match = MATCHED_IDS.search(result.stdout + result.stderr)
        if match is None:
            raise RuntimeError(f"ocloc ids {target}: no GFX IP version in its output")
        families.setdefault(match.group(1), set()).add(aot.family(target))
    return families


@dataclass
class UnitResult:
    """One rewritten compile: its diagnostics, its images and their kernels in scratch."""

    source: str
    diagnostics: list[str]
    images: list[str] = field(default_factory=list)  # the GFX IP of each image
    in_scratch: list[scratch.KernelScratch] = field(default_factory=list)


def compile_unit(build_dir: Path, source: str, argv: list[str], output: str) -> UnitResult:
    """Run one rewritten compile and read the kernel images of its object."""
    result = subprocess.run(  # noqa: S603 -- the build's own compile command, no shell
        argv, cwd=build_dir, capture_output=True, text=True, timeout=3000, check=False
    )
    if result.returncode != 0:
        text = result.stdout + result.stderr
        lines = [line.strip() for line in text.splitlines() if DIAGNOSTIC.search(line)]
        return UnitResult(source, lines or [text.strip()[-2000:] or f"exit {result.returncode}"])
    images, in_scratch = scratch.object_scratch(Path(output).read_bytes())
    return UnitResult(source, [], images, in_scratch)


def scratch_failures(results: list[UnitResult], ip_families: dict[str, set[str]]) -> list[str]:
    """Kernels in scratch memory on any target, and images that do not cover the list."""
    found = [entry for result in results for entry in result.in_scratch]
    images = [ip for result in results for ip in result.images]
    failures, notes = scratch.judge(found, ip_families)
    print(
        f"read {len(images)} kernel images for {len(set(images))} GFX IP versions; "
        f"kernels in scratch memory: {len(found)}, "
        f"{len(found) - len(failures)} of them in KNOWN_SCRATCH"
    )
    if set(images) != set(ip_families):
        failures.append(
            f"kernel images for GFX IP {sorted(set(images))}; the default list has {sorted(ip_families)}"
        )
    for note in notes:
        print(f"note: {note}")
    return failures


def compile_failures(
    build_dir: Path, targets: list[str], jobs: int, workdir: Path, ip_families: dict[str, set[str]]
) -> list[str]:
    """Every unit of the build that does not compile for `targets`, and every scratch kernel."""
    found = units((build_dir / "build.ninja").read_text(encoding="utf-8"))
    tasks = []
    for index, (_output, source, argv) in enumerate(found):
        output = str(workdir / f"{index}.o")
        tasks.append((source, for_targets(argv, targets, output), output))
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(lambda task: compile_unit(build_dir, *task), tasks))
    failures = [f"{r.source}: {line}" for r in results for line in r.diagnostics]
    print(f"compiled {len(tasks)} SYCL translation units for {len(targets)} targets")
    if not failures:
        failures += scratch_failures(results, ip_families)
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
            print(
                "scratch check not run: this build's images are compressed in its objects; "
                "run the suite from a build configured with another target list"
            )
        else:
            ip_families = target_ips(ocloc, targets)
            failures += compile_failures(build_dir, targets, jobs, workdir, ip_families)
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
