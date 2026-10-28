#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Compile extracted C code samples from documentation against public headers.

Usage: test_api_doc_samples.py --cc=<word>... --cc-syntax=<gcc|msvc> --cc-id=<id>
       <core include dir> <build include dir> <docs dir> <extractor script>
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

from meson_cc import take_cc, take_value

TIMEOUT = 120


def build_compile_cmd(
    cc: list[str], msvc_syntax: bool, core_inc: Path, build_inc: Path, sample: Path
) -> list[str]:
    """Construct syntax-only compilation command."""
    if msvc_syntax:
        return [
            *cc,
            "/nologo",
            "/Zs",
            f"/I{core_inc}",
            f"/I{build_inc}",
            str(sample),
        ]
    return [
        *cc,
        "-fsyntax-only",
        f"-I{core_inc}",
        f"-I{build_inc}",
        str(sample),
    ]


def run_extractor(
    extractor: Path, docs_dir: Path, out_dir: Path
) -> subprocess.CompletedProcess[str]:
    """Run the sample extractor script into out_dir."""
    cmd = [
        sys.executable,
        str(extractor),
        "--docs-dir",
        str(docs_dir),
        "--extract",
        str(out_dir),
    ]
    return subprocess.run(  # noqa: S603
        cmd, capture_output=True, text=True, check=False, timeout=TIMEOUT
    )


def compile_samples(
    cc: list[str],
    msvc_syntax: bool,
    diag_stdout: bool,
    core_inc: Path,
    build_inc: Path,
    samples: list[Path],
) -> int:
    """Compile each extracted sample with syntax-only flags."""
    for sample in samples:
        cmd = build_compile_cmd(cc, msvc_syntax, core_inc, build_inc, sample)
        res = subprocess.run(  # noqa: S603
            cmd, capture_output=True, text=True, check=False, timeout=TIMEOUT
        )
        if res.returncode != 0:
            diag = res.stdout if diag_stdout else res.stderr
            sys.stderr.write(f"FAIL: {sample.name} failed compilation:\n{diag}\n")
            return 1
    return 0


def main() -> int:
    """Extract and compile documentation samples."""
    cc = take_cc(sys.argv)
    msvc_syntax = take_value(sys.argv, "cc-syntax") == "msvc"
    diag_stdout = take_value(sys.argv, "cc-id") == "msvc"
    core_inc = Path(sys.argv[1])
    build_inc = Path(sys.argv[2])
    docs_dir = Path(sys.argv[3])
    extractor = Path(sys.argv[4])
    del sys.argv[1:5]

    with tempfile.TemporaryDirectory(prefix="vmafx-doc-samples-") as tmp:
        out_dir = Path(tmp)
        proc = run_extractor(extractor, docs_dir, out_dir)
        if proc.returncode != 0:
            sys.stderr.write(f"extractor failed:\n{proc.stdout}\n{proc.stderr}\n")
            return 1

        samples = sorted(out_dir.glob("*.c"))
        if not samples:
            print("SKIP: no compile-marked doc samples found")
            return 77

        status = compile_samples(cc, msvc_syntax, diag_stdout, core_inc, build_inc, samples)
        if status != 0:
            return status

    print(f"OK: compiled {len(samples)} documentation sample(s) successfully")
    return 0


if __name__ == "__main__":
    sys.exit(main())
