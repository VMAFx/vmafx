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


def check_symbols(symbols: str, floor: tuple[int, ...]) -> str | None:
    """Report an incompatible dynamic symbol table."""
    versions = set(re.findall(r"\bGLIBC_([A-Za-z0-9_.]+)", symbols))
    if not versions:
        return "no GLIBC symbol versions found"
    for version in sorted(versions):
        if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", version):
            return f"unsupported GLIBC_{version} requirement"
        if tuple(map(int, version.split("."))) > floor:
            return f"GLIBC_{version} exceeds glibc {'.'.join(map(str, floor))}"
    return None


def main(argv: list[str]) -> int:
    """Check the supplied CLI and every staged library using objdump -T."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("floor", help="maximum glibc version, e.g. 2.28")
    parser.add_argument("files", nargs="+", type=Path, help="staged ELF files")
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[0-9]+\.[0-9]+", args.floor):
        parser.error("floor must have the form MAJOR.MINOR")
    floor = tuple(map(int, args.floor.split(".")))
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
            problem = check_symbols(result.stdout, floor)
        except (OSError, subprocess.SubprocessError) as error:
            problem = f"objdump failed: {error}"
        if problem:
            print(f"ERROR: {binary}: {problem}", file=sys.stderr)
            failed = True
        else:
            print(f"OK: {binary}: GLIBC versions <= {args.floor}")
    return int(failed)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
