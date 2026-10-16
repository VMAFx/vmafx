#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""A Meson test proving a caller cannot set a colour on VmafPicture through the public header.

Netflix upstream 0b94628 added `VmafColor color` as the last member of VmafPicture;
the fork deliberately has no such member (ADR-1822); colour goes through
vmaf_set_input_colorimetry() or vmaf_picture_convert_context_init_with_color().

Usage: test_picture_no_color_member.py --cc=<word>... --cc-syntax=<gcc|msvc> --cc-id=<id> <public include dir>
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from meson_cc import take_cc, take_value

TIMEOUT = 120  # seconds per compile (HISS-02)
CC = take_cc(sys.argv)
MSVC_SYNTAX = take_value(sys.argv, "cc-syntax") == "msvc"
# cl.exe writes compile diagnostics to stdout; clang-cl, GCC and clang to stderr.
DIAGNOSTICS_ON_STDOUT = take_value(sys.argv, "cc-id") == "msvc"
INCLUDE = Path(sys.argv[1])
del sys.argv[1:2]


def compile_argv(source: Path, obj: Path) -> list[str]:
    """The compile command: syntax-aware object file compilation."""
    if MSVC_SYNTAX:
        return [
            *CC,
            "/nologo",
            "/c",
            f"/I{INCLUDE}",
            str(source),
            f"/Fo{obj}",
        ]
    return [*CC, "-c", f"-I{INCLUDE}", str(source), "-o", str(obj)]


def diagnostics(done: subprocess.CompletedProcess[str]) -> str:
    """The stream the compiler writes its diagnostics to: stdout for cl.exe, stderr otherwise."""
    return done.stdout if DIAGNOSTICS_ON_STDOUT else done.stderr


def compile_snippet(code: str) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory() as raw:
        source = Path(raw) / "snippet.c"
        source.write_text(code, encoding="utf-8")
        obj = Path(raw) / ("snippet.obj" if MSVC_SYNTAX else "snippet.o")
        argv = compile_argv(source, obj)
        return subprocess.run(  # noqa: S603
            argv, capture_output=True, text=True, check=False, timeout=TIMEOUT
        )


class PictureNoColorMemberTest(unittest.TestCase):
    def test_control_picture_definition_usable(self) -> None:
        code = (
            "#include <libvmaf/picture.h>\n"
            "int f(void){ VmafPicture p = {0}; return p.priv == 0 && p.ref == 0; }\n"
        )
        done = compile_snippet(code)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    def test_member_write_color_fails(self) -> None:
        code = (
            "#include <libvmaf/picture.h>\n"
            "int f(void){ VmafPicture p = {0}; p.color.range = VMAF_COLOR_RANGE_LIMITED; return 0; }\n"
        )
        done = compile_snippet(code)
        self.assertNotEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn("color", diagnostics(done), done.stdout + done.stderr)

    def test_designated_init_color_fails(self) -> None:
        code = (
            "#include <libvmaf/picture.h>\n"
            "int f(void){ VmafPicture p = { .color = { VMAF_COLOR_RANGE_LIMITED } }; (void)p; return 0; }\n"
        )
        done = compile_snippet(code)
        self.assertNotEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn("color", diagnostics(done), done.stdout + done.stderr)

    def test_vmaf_color_type_still_usable(self) -> None:
        code = (
            "#include <libvmaf/picture.h>\n"
            "int f(void) {\n"
            "    VmafColor c = { VMAF_COLOR_RANGE_LIMITED, VMAF_COLOR_PRIMARIES_BT709,\n"
            "                    VMAF_COLOR_TRC_BT709, VMAF_COLOR_MATRIX_BT709 };\n"
            "    return c.range == VMAF_COLOR_RANGE_LIMITED;\n"
            "}\n"
        )
        done = compile_snippet(code)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)


if __name__ == "__main__":
    unittest.main(argv=sys.argv[:1])
