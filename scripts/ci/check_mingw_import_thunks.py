#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check that MinGW ``*.dll.a`` import libraries contain function thunks (ADR-3397).

When a MinGW GCC DLL is linked with LTO, GNU ``ld --out-implib`` emits ``I __imp_<fn>``
pointer entries and ``I __nm_<fn>`` data aliases, omitting the ``T <fn>`` function
thunks.  An executable linked against that import library auto-imports every function
call as data, creating a 32-bit PEI runtime pseudo-relocation per call site that
aborts before ``main`` under 64-bit high-entropy ASLR (VMAFx/vmafx#3257).

For every ``*.dll.a`` under ``<build>/src``, this script runs ``nm`` and counts
``__imp_`` entries and `` T `` thunk symbols.  It fails (exit 1) when no ``*.dll.a``
files are found, when ``nm`` fails, or when any archive has
``thunk_count < imp_count * MIN_THUNK_RATIO`` (missing function thunks).
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from collections.abc import Callable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import (
    CommandOutputLimitExceeded,
    CommandTimedOut,
    CommandValidationError,
    executable_argument,
)
from scripts.lib.safe_subprocess import (
    run as run_command,
)

NM_TIMEOUT_SECONDS = 60.0
NM_MAX_OUTPUT_BYTES = 64 * 1_048_576
MIN_THUNK_RATIO = 0.5
"""Minimum ratio of function thunks (``T``) to import slots (``__imp_``).

Data exports have no thunk by design. Broken LTO DLL builds emit few or no thunks
(measured 0/78 for libvmaf and 48/1183 for libvmafx), whereas good builds emit thunks
for all or almost all functions (measured 78/78 for libvmaf and 1015/1136 for libvmafx).
"""
Runner = Callable[[str, Path], tuple[int, str, str]]


def resolve_nm_tool(explicit: str | None) -> str:
    """Resolve the ``nm`` executable from ``--nm``, ``$NM``, or ``nm`` on ``PATH``."""
    candidate = explicit or os.environ.get("NM") or "nm"
    executable = executable_argument(candidate, Path.cwd())
    found = shutil.which(executable)
    if found is None:
        raise ValueError(f"nm binary not found: {candidate!r}")
    return str(Path(found).resolve(strict=True))


def default_run_nm(nm_tool: str, archive: Path) -> tuple[int, str, str]:
    """Run ``nm`` on ``archive`` with a timeout and bounded output."""
    try:
        resolved = resolve_nm_tool(nm_tool)
        proc = run_command(
            [resolved, str(archive)],
            allowed_executables=(resolved,),
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=NM_TIMEOUT_SECONDS,
            max_output_bytes=NM_MAX_OUTPUT_BYTES,
        )
    except (
        OSError,
        ValueError,
        CommandValidationError,
        CommandTimedOut,
        CommandOutputLimitExceeded,
    ) as exc:
        return 1, "", str(exc)
    return proc.returncode, proc.stdout, proc.stderr


def count_symbols(nm_output: str) -> tuple[int, int]:
    """Return ``(imp_count, thunk_count)`` from ``nm`` text output."""
    imp_count = 0
    thunk_count = 0
    for line in nm_output.splitlines():
        if " __imp_" in line:
            imp_count += 1
        if " T " in line:
            thunk_count += 1
    return imp_count, thunk_count


def check_build_dir(
    build_dir: Path,
    nm_tool: str | None = None,
    runner: Runner = default_run_nm,
) -> int:
    """Inspect ``<build_dir>/src/*.dll.a``; return 0 on pass, 1 on failure."""
    src_dir = build_dir / "src"
    archives = sorted(src_dir.glob("*.dll.a")) if src_dir.is_dir() else []
    if not archives:
        print(
            f"check_mingw_import_thunks: {src_dir}: no *.dll.a import libraries found",
            file=sys.stderr,
        )
        return 1

    tool = nm_tool or os.environ.get("NM") or "nm"
    failed = False
    for archive in archives:
        rc, stdout, stderr = runner(tool, archive)
        if rc != 0:
            detail = stderr.strip() or stdout.strip() or f"exit {rc}"
            print(
                f"check_mingw_import_thunks: {archive}: nm ({tool}) failed (exit {rc}): {detail}",
                file=sys.stderr,
            )
            failed = True
            continue
        imp_count, thunk_count = count_symbols(stdout)
        if imp_count > 0 and thunk_count < imp_count * MIN_THUNK_RATIO:
            ratio = thunk_count / imp_count
            print(
                f"check_mingw_import_thunks: FAIL {archive}: __imp_={imp_count}, T={thunk_count} "
                f"(ratio {ratio:.3f} < {MIN_THUNK_RATIO}; missing function thunks; "
                "LTO-linked DLL import library, VMAFx/vmafx#3257, ADR-3397)",
                file=sys.stderr,
            )
            failed = True
        else:
            print(f"check_mingw_import_thunks: OK {archive}: __imp_={imp_count}, T={thunk_count}")
    return 1 if failed else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "build_dir",
        type=Path,
        help="Meson build directory containing src/*.dll.a import libraries",
    )
    parser.add_argument(
        "--nm",
        default=None,
        help="nm binary to use (default: $NM or nm)",
    )
    args = parser.parse_args(argv)
    return check_build_dir(args.build_dir, nm_tool=args.nm)


if __name__ == "__main__":
    sys.exit(main())
