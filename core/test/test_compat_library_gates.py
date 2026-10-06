#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Gates of the library split (ADR-1852 decision D3, RC4 WP6), against this build.

1. Link completeness: a shared library that calls an exported vmafx_ function
   links against libvmafx.so.1 with the no-undefined rule libvmaf.so.3 is
   linked with; one that reaches an engine symbol (a renamed libvmaf body,
   hidden by the version script) or a libvmaf name does not. So the compat
   library builds only while it uses the public API.
2. The export checks refuse a planted missing symbol: check_exported_symbols
   fails on libvmaf.so.3 when the compat list loses a row and on libvmafx.so.1
   when the VMAFx list loses one.

Usage: test_compat_library_gates.py <cc> <libvmafx.so> <libvmaf.so> <src dir> <backends> <features>
ELF only; exits 77 (skipped) with the reason without nm or a working compiler.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TIMEOUT = 120  # seconds per compiler or checker run (HISS-02)
CC, LIBVMAFX, LIBVMAF, SRC = (Path(a) for a in sys.argv[1:5])
BUILD = ["--backends", sys.argv[5], "--features", sys.argv[6]]
del sys.argv[1:7]
CHECKER = Path(__file__).resolve().parent / "check_exported_symbols.py"


def link(tmp: Path, call: str) -> subprocess.CompletedProcess[str]:
    """Link a shared library calling `call` against libvmafx.so.1, no undefined symbols."""
    source = tmp / "probe.c"
    source.write_text(f"extern int {call}(void);\nint probe(void) {{ return {call}(); }}\n")
    argv = [
        str(CC),
        "-shared",
        "-fPIC",
        str(source),
        "-o",
        str(tmp / "libprobe.so"),
        f"-L{LIBVMAFX.parent}",
        f"-l:{LIBVMAFX.name}",
        "-Wl,--no-undefined",
    ]
    # The build's compiler and paths from Meson, no shell.
    return subprocess.run(  # noqa: S603
        argv, capture_output=True, text=True, check=False, timeout=TIMEOUT
    )


def check(library: str, path: Path, vmafx_list: Path, compat_list: Path) -> int:
    argv = [
        sys.executable,
        str(CHECKER),
        "--library",
        library,
        "--vmafx-symbols",
        str(vmafx_list),
        "--compat-symbols",
        str(compat_list),
        *BUILD,
        str(path),
    ]
    # This interpreter, the checker beside this file, paths from Meson; no shell.
    return subprocess.run(  # noqa: S603
        argv, capture_output=True, text=True, check=False, timeout=TIMEOUT
    ).returncode


def without_row(source: Path, target: Path, prefix: str) -> None:
    """Copy a symbol list without its first row that starts with `prefix`."""
    lines = source.read_text(encoding="utf-8").splitlines(keepends=True)
    index = next(i for i, line in enumerate(lines) if line.startswith(prefix))
    target.write_text("".join(lines[:index] + lines[index + 1 :]), encoding="utf-8")


class LinkCompletenessTest(unittest.TestCase):
    def test_exported_vmafx_function_links(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            done = link(Path(raw), "vmafx_abi_version")
        self.assertEqual(done.returncode, 0, done.stderr)

    def test_engine_symbol_does_not_link(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            done = link(Path(raw), "vmaf_engine_init")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("vmaf_engine_init", done.stderr)

    def test_libvmaf_name_is_not_in_libvmafx(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            done = link(Path(raw), "vmaf_init")
        self.assertNotEqual(done.returncode, 0)


class PlantedMissingSymbolTest(unittest.TestCase):
    def test_lists_match_this_build(self) -> None:
        vmafx, compat = SRC / "vmafx_symbols.txt", SRC / "libvmaf_symbols.txt"
        self.assertEqual(check("vmaf", LIBVMAF, vmafx, compat), 0)
        self.assertEqual(check("vmafx", LIBVMAFX, vmafx, compat), 0)

    def test_compat_list_missing_a_row_fails(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            planted = Path(raw) / "libvmaf_symbols.txt"
            without_row(SRC / "libvmaf_symbols.txt", planted, "vmaf_init ")
            self.assertEqual(check("vmaf", LIBVMAF, SRC / "vmafx_symbols.txt", planted), 1)

    def test_vmafx_list_missing_a_row_fails(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            planted = Path(raw) / "vmafx_symbols.txt"
            without_row(SRC / "vmafx_symbols.txt", planted, "vmafx_submit ")
            self.assertEqual(check("vmafx", LIBVMAFX, planted, SRC / "libvmaf_symbols.txt"), 1)


if __name__ == "__main__":
    if shutil.which("nm") is None:
        print("SKIP: nm not found")
        raise SystemExit(77)
    unittest.main()
