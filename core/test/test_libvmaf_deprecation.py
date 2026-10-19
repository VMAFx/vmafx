#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""libvmaf deprecation warnings are opt-in in 1.0 (ADR-1852 decision D7, RC4 WP6).

- Every exported function of core/include/libvmaf/*.h carries
  VMAF_DEPRECATED("use <target>") with the target core/api/vmafx.toml names
  for it (a declaration without one, or with another target, fails).
- A consumer that defines VMAF_ENABLE_DEPRECATION_WARNINGS gets a warning
  naming the replacement (an error under -Werror); one that does not compiles
  clean under -Werror, as upstream FFmpeg must; the library itself
  (VMAF_BUILDING_LIBVMAF) never warns.

Usage: test_libvmaf_deprecation.py --cc=<word>... --cc-syntax=<gcc|msvc> --cc-id=<id>
       <public include dir> <definition>
(--cc: the compiler command as Meson runs it, launcher included; --cc-syntax and --cc-id:
Meson's cc.get_argument_syntax() and cc.get_id(); meson_cc.py)

With the msvc syntax (cl.exe, clang-cl) the compile takes /W3 /WX, the level of the
project's warning_level=2 and its warnings-as-errors switch; `-Werror` is an invalid /W
level to cl.exe (the Windows ARM64 MSVC leg failed on "D8021 : invalid numeric argument
'/Werror'"). cl.exe writes compile diagnostics to standard output, its banner and
command-line errors to standard error, so its diagnostics are read from standard output;
every other compiler's from standard error.
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import tomllib
from meson_cc import take_cc, take_value

TIMEOUT = 120  # seconds per compile (HISS-02)
CC = take_cc(sys.argv)
MSVC_SYNTAX = take_value(sys.argv, "cc-syntax") == "msvc"
# cl.exe writes compile diagnostics to stdout; clang-cl, GCC and clang to stderr.
DIAGNOSTICS_ON_STDOUT = take_value(sys.argv, "cc-id") == "msvc"
INCLUDE, DEFINITION = (Path(a) for a in sys.argv[1:3])
del sys.argv[1:3]
DECLARATION = re.compile(
    r'(?:VMAF_DEPRECATED\("(?P<msg>[^"]*)"\)\s*)?VMAF_EXPORT\b[^;(]*?\b(?P<name>vmaf_\w+)\s*\(',
    re.S,
)
CONSUMER = """#include <libvmaf/libvmaf.h>
#include <stddef.h>
int main(void) { return vmaf_version() == NULL; }
"""


def targets() -> dict[str, str]:
    with DEFINITION.open("rb") as handle:
        return {c["name"]: c["target"] for c in tomllib.load(handle)["compat"]}


def findings(text: str, expected: dict[str, str]) -> list[str]:
    """Declarations without the marker, or naming another target than the definition."""
    out = []
    for match in DECLARATION.finditer(re.sub(r"/\*.*?\*/", "", text, flags=re.S)):
        name, msg = match.group("name"), match.group("msg")
        target = expected.get(name)
        if target is None:
            out.append(f"{name}: exported but not in core/api/vmafx.toml [[compat]]")
        elif msg is None:
            out.append(f"{name}: no VMAF_DEPRECATED marker")
        elif target != "compat-internal" and msg != f"use {target}":
            out.append(f"{name}: marker says {msg!r}, the definition names {target}")
    return out


def compile_argv(source: Path, obj: Path, defines: tuple[str, ...]) -> list[str]:
    """The compile command: warnings on and made errors, in the compiler's own syntax.

    `-D<name>` is valid for cl.exe as for GCC and clang.
    """
    if MSVC_SYNTAX:
        return [
            *CC,
            "/nologo",
            "/c",
            "/W3",
            "/WX",
            f"/I{INCLUDE}",
            *defines,
            str(source),
            f"/Fo{obj}",
        ]
    return [*CC, "-c", "-Wall", "-Werror", f"-I{INCLUDE}", *defines, str(source), "-o", str(obj)]


def diagnostics(done: subprocess.CompletedProcess[str]) -> str:
    """The stream the compiler writes its diagnostics to: stdout for cl.exe, stderr otherwise."""
    return done.stdout if DIAGNOSTICS_ON_STDOUT else done.stderr


def compile_consumer(*defines: str) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory() as raw:
        source = Path(raw) / "consumer.c"
        source.write_text(CONSUMER, encoding="utf-8")
        argv = compile_argv(source, Path(raw) / "consumer.o", defines)
        # The build's compiler and a source this test wrote; no shell.
        return subprocess.run(  # noqa: S603
            argv, capture_output=True, text=True, check=False, timeout=TIMEOUT
        )


class MarkerTest(unittest.TestCase):
    def test_every_exported_function_names_its_successor(self) -> None:
        expected = targets()
        seen: list[str] = []
        problems: list[str] = []
        for header in sorted((INCLUDE / "libvmaf").glob("*.h")):
            text = header.read_text(encoding="utf-8")
            seen += [m.group("name") for m in DECLARATION.finditer(text)]
            problems += findings(text, expected)
        self.assertEqual(problems, [])
        self.assertEqual(sorted(seen), sorted(expected))

    def test_planted_defects_are_found(self) -> None:
        expected = {"vmaf_a": "vmafx_a", "vmaf_b": "vmafx_b"}
        text = (
            'VMAF_DEPRECATED("use vmafx_a")\nVMAF_EXPORT int vmaf_a(void);\n'
            "VMAF_EXPORT int vmaf_b(void);\n"
            'VMAF_DEPRECATED("use vmafx_z")\nVMAF_EXPORT int vmaf_c(void);\n'
        )
        self.assertEqual(
            findings(text, expected),
            [
                "vmaf_b: no VMAF_DEPRECATED marker",
                "vmaf_c: exported but not in core/api/vmafx.toml [[compat]]",
            ],
        )
        wrong = 'VMAF_DEPRECATED("use vmafx_z")\nVMAF_EXPORT int vmaf_a(void);\n'
        self.assertEqual(
            findings(wrong, expected),
            ["vmaf_a: marker says 'use vmafx_z', the definition names vmafx_a"],
        )


class CompileTest(unittest.TestCase):
    def test_default_build_has_no_warning(self) -> None:
        done = compile_consumer()
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    def test_opt_in_names_the_replacement(self) -> None:
        done = compile_consumer("-DVMAF_ENABLE_DEPRECATION_WARNINGS")
        self.assertNotEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn("vmafx_version_string", diagnostics(done), done.stdout + done.stderr)

    def test_library_itself_never_warns(self) -> None:
        done = compile_consumer("-DVMAF_ENABLE_DEPRECATION_WARNINGS", "-DVMAF_BUILDING_LIBVMAF")
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)


if __name__ == "__main__":
    unittest.main()
