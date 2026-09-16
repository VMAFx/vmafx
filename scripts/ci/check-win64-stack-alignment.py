#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Reject Win64 objects that assume more stack alignment than the ABI gives.

The Microsoft x64 calling convention guarantees only **16-byte** stack
alignment at a call boundary, and its unwind contract forbids the
``and $-32, %rsp`` / ``and $-64, %rsp`` frame realignment that gcc emits
freely on SysV.  gcc's MinGW target nevertheless allocates 32/64-byte-aligned
spill slots for ``ymm`` / ``zmm`` registers and addresses them as a fixed
offset from ``%rsp``, so a function that spills a wide vector register
executes ``vmovaps %zmm28,0x1a0(%rsp)`` against an address that is 64-byte
aligned only when the caller happened to leave ``%rsp`` at the right residue.
When it is not, the CPU raises a general-protection fault, which Windows
reports as an access violation on address ``0xFFFFFFFFFFFFFFFF`` — there is no
faulting linear address for a #GP, so the field is filled with -1.

That is a crash in shipped code that no amount of source-level review catches,
and the CI runners do not expose AVX-512, so the Windows test leg cannot catch
it either.  This gate reads the built objects instead: it disassembles each
one, and fails if any function performs a 32- or 64-byte-aligned vector access
relative to ``%rsp`` / ``%rbp`` without that function having realigned its
frame first.

128-bit (``xmm``) aligned accesses are fine and are not reported: 16 bytes is
exactly what the ABI promises.

See ADR-1254 and docs/research/2061-win64-cannot-realign-the-stack.md.

Exit codes: 0 clean, 1 at least one unsafe access found, 2 usage / IO error.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

# `0000000000004414 <ssim_accumulate_avx512>:`
_FUNC_RE = re.compile(r"^[0-9a-f]+ <(?P<name>[^>]+)>:$")

# gcc realigns either by masking %rsp directly, or by rounding a scratch
# pointer up inside an oversized frame (`lea 0x3f(%rsp),%rax; and $-64,%rax`).
# Either form means the function knows its alignment is not guaranteed and has
# dealt with it, so anything it does afterwards is its own business.
_REALIGN_RE = re.compile(r"and\s+\$0xffffffffffffff(?:c0|e0),%r|lea\s+0x(?:3f|1f)\(%rsp\),%r")

# An ALIGNED wide vector move whose memory operand is frame-relative, in either
# direction (store to the slot, reload from it).
_ALIGNED_VEC_RE = re.compile(
    r"\bv(?:movap[sd]|movdqa(?:32|64)?)\b[^#]*?"
    r"(?:%[zy]mm\d+,\s*-?(?:0x[0-9a-f]+)?\(%r(?:sp|bp)\)"
    r"|-?(?:0x[0-9a-f]+)?\(%r(?:sp|bp)\),\s*%[zy]mm\d+)"
)


def _objdump_binary(explicit: str | None) -> str:
    for candidate in (explicit, "x86_64-w64-mingw32-objdump", "objdump"):
        if candidate and shutil.which(candidate):
            return candidate
    raise SystemExit(
        "error: no usable objdump found (tried --objdump, x86_64-w64-mingw32-objdump, objdump)"
    )


def _disassemble(objdump: str, path: Path) -> str:
    proc = subprocess.run(
        [objdump, "-d", "--no-show-raw-insn", str(path)],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        return ""
    return proc.stdout


def scan_disassembly(text: str) -> list[tuple[str, str]]:
    """Return ``(function, instruction)`` for every unsafe aligned access."""
    findings: list[tuple[str, str]] = []
    func = "<toplevel>"
    realigned = False
    pending: list[str] = []

    def flush() -> None:
        if pending and not realigned:
            findings.extend((func, insn) for insn in pending)
        pending.clear()

    for raw in text.splitlines():
        line = raw.strip()
        match = _FUNC_RE.match(line)
        if match:
            flush()
            func = match.group("name")
            realigned = False
            continue
        if not line:
            flush()
            realigned = False
            continue
        if _REALIGN_RE.search(line):
            realigned = True
            continue
        if _ALIGNED_VEC_RE.search(line):
            pending.append(line)
    flush()
    return findings


def iter_targets(roots: list[Path]) -> list[Path]:
    suffixes = {".o", ".obj", ".exe", ".dll", ".a"}
    targets: list[Path] = []
    for root in roots:
        if root.is_file():
            targets.append(root)
            continue
        targets.extend(p for p in sorted(root.rglob("*")) if p.suffix in suffixes and p.is_file())
    return targets


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "paths",
        nargs="+",
        type=Path,
        help="build directories, object files or executables to scan",
    )
    parser.add_argument("--objdump", help="objdump to use (default: autodetect)")
    parser.add_argument(
        "--quiet", action="store_true", help="only print findings, not the scan summary"
    )
    args = parser.parse_args(argv)

    objdump = _objdump_binary(args.objdump)
    targets = iter_targets(args.paths)
    if not targets:
        print("error: no object files or executables found in", *args.paths, file=sys.stderr)
        return 2

    findings: list[tuple[Path, str, str]] = []
    for target in targets:
        text = _disassemble(objdump, target)
        if not text:
            continue
        findings.extend((target, fn, insn) for fn, insn in scan_disassembly(text))

    if not args.quiet:
        print(f"scanned {len(targets)} file(s) with {objdump}")

    if findings:
        print(
            "\nWin64 stack-alignment violation: an aligned 256/512-bit vector access\n"
            "is addressed off %rsp/%rbp in a frame that was never realigned. The MS x64\n"
            "ABI only guarantees 16-byte alignment, so this faults (#GP, reported by\n"
            "Windows as a read of 0xFFFFFFFFFFFFFFFF) whenever the caller's stack is not\n"
            "already aligned to the access width. See ADR-1254.\n",
            file=sys.stderr,
        )
        for path, func, insn in findings:
            print(f"  {path}: {func}: {insn}", file=sys.stderr)
        print(
            f"\n{len(findings)} unsafe access(es). Reduce vector register pressure in the\n"
            "listed function(s) so the compiler does not need a wide spill slot.",
            file=sys.stderr,
        )
        return 1

    if not args.quiet:
        print("no Win64 stack-alignment violations")
    return 0


if __name__ == "__main__":
    sys.exit(main())
