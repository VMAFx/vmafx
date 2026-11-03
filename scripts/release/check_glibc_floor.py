#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Reject release ELF files that require glibc newer than the declared floor."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path


def _check_family(
    symbols: str,
    prefix: str,
    label: str,
    floor: tuple[int, ...],
) -> str | None:
    versions = set(re.findall(rf"\b{prefix}_([A-Za-z0-9_.]+)", symbols))
    for version in sorted(versions):
        if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", version):
            return f"unsupported {prefix}_{version} requirement"
        if tuple(map(int, version.split("."))) > floor:
            return f"{prefix}_{version} exceeds {label} {'.'.join(map(str, floor))}"
    return None


def check_symbols(
    symbols: str,
    floor: tuple[int, ...],
    cxx_floor: tuple[int, ...] = (3, 4, 25),
    cxxabi_floor: tuple[int, ...] = (1, 3, 11),
) -> str | None:
    """Report an incompatible dynamic symbol table."""
    if not re.search(r"\bGLIBC_[A-Za-z0-9_.]+", symbols):
        return "no GLIBC symbol versions found"
    families = (
        ("GLIBC", "glibc", floor),
        ("GLIBCXX", "libstdc++", cxx_floor),
        ("CXXABI", "CXXABI", cxxabi_floor),
    )
    for prefix, label, max_floor in families:
        problem = _check_family(symbols, prefix, label, max_floor)
        if problem:
            return problem
    return None


def main(argv: list[str]) -> int:
    """Check the supplied CLI and every staged library using objdump -T."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("floor", help="maximum glibc version, e.g. 2.28")
    parser.add_argument(
        "--cxx-floor",
        default="3.4.25",
        help="maximum libstdc++ GLIBCXX version, e.g. 3.4.25",
    )
    parser.add_argument(
        "--cxxabi-floor",
        default="1.3.11",
        help="maximum CXXABI version, e.g. 1.3.11",
    )
    parser.add_argument("files", nargs="+", type=Path, help="staged ELF files")
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[0-9]+\.[0-9]+", args.floor):
        parser.error("floor must have the form MAJOR.MINOR")
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", args.cxx_floor):
        parser.error("cxx-floor must have numeric form, e.g. 3.4.25")
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", args.cxxabi_floor):
        parser.error("cxxabi-floor must have numeric form, e.g. 1.3.11")
    floor = tuple(map(int, args.floor.split(".")))
    cxx_floor = tuple(map(int, args.cxx_floor.split(".")))
    cxxabi_floor = tuple(map(int, args.cxxabi_floor.split(".")))
    objdump = shutil.which("objdump")
    if objdump is None:
        print("ERROR: objdump is not on PATH", file=sys.stderr)
        return 1
    failed = False
    for binary in args.files:
        try:
            result = subprocess.run(  # noqa: S603 -- resolved objdump, argv only, no shell
                [objdump, "-T", str(binary)],
                capture_output=True,
                text=True,
                check=True,
                timeout=30,
            )
            problem = check_symbols(result.stdout, floor, cxx_floor, cxxabi_floor)
        except (OSError, subprocess.SubprocessError) as error:
            problem = f"objdump failed: {error}"
        if problem:
            print(f"ERROR: {binary}: {problem}", file=sys.stderr)
            failed = True
        else:
            print(
                f"OK: {binary}: GLIBC <= {args.floor}, GLIBCXX <= {args.cxx_floor}, "
                f"CXXABI <= {args.cxxabi_floor}"
            )
    return int(failed)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
