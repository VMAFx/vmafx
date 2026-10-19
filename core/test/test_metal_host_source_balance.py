#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every Objective-C++ Metal host file closes what it opens.

The Metal host code (core/src/metal/*.mm, core/src/feature/metal/*.mm) is
compiled only on macOS, and the macOS legs are not required checks, so a
structural slip there lands on master unseen: #2222 opened a second anonymous
namespace in float_motion_metal.mm and closed one, and every macOS Metal build
stopped at `float_motion_metal.mm:869:19: error: expected '}'`
(T-METAL-FLOAT-MOTION-NAMESPACE-2026-10-06).

The test reads each file with comments, string and character literals removed
and requires the braces to balance, never to close below zero, and every
`namespace {` to have its `} // namespace`. It does not compile anything.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

CORE = Path(__file__).resolve().parents[1]
SOURCES = ("src/metal", "src/feature/metal")
TOKEN = re.compile(
    r"""//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'|@"(?:\\.|[^"\\\n])*\"""",
    re.S,
)


def code_of(text: str) -> str:
    """The text with comments and literals blanked out."""
    return TOKEN.sub(" ", text)


def balance_errors(name: str, text: str) -> list[str]:
    code = code_of(text)
    errors = []
    depth = 0
    for line_no, line in enumerate(code.splitlines(), start=1):
        for ch in line:
            depth += {"{": 1, "}": -1}.get(ch, 0)
            if depth < 0:
                errors.append(f"{name}:{line_no}: '}}' closes more than was opened")
                depth = 0
    if depth:
        errors.append(f"{name}: {depth} '{{' never closed")
    opened = len(re.findall(r"\bnamespace\s*\{", code))
    closed = len(re.findall(r"\}\s*//\s*namespace\b", text))
    if opened != closed:
        errors.append(f"{name}: {opened} 'namespace {{' but {closed} '}} // namespace'")
    return errors


def metal_host_files(root: Path) -> list[Path]:
    return sorted(p for sub in SOURCES for p in (root / sub).glob("*.mm"))


class MetalHostSourceBalance(unittest.TestCase):
    def test_every_metal_host_file_balances(self) -> None:
        files = metal_host_files(CORE)
        self.assertTrue(files, "no Metal host file found")
        errors = []
        for path in files:
            errors += balance_errors(path.relative_to(CORE).as_posix(), path.read_text("utf-8"))
        self.assertEqual(errors, [])

    def test_an_unclosed_namespace_is_reported(self) -> None:
        # master's float_motion_metal.mm in miniature.
        text = "namespace {\nstruct S { int a; };\n\nnamespace {\nint f() { return 0; }\n} // namespace\n"
        self.assertEqual(
            balance_errors("x.mm", text),
            ["x.mm: 1 '{' never closed", "x.mm: 2 'namespace {' but 1 '} // namespace'"],
        )

    def test_braces_in_comments_and_literals_do_not_count(self) -> None:
        text = "int f() { /* { */ const char *s = \"}\"; char c = '{'; return 0; } // }\n"
        self.assertEqual(balance_errors("y.mm", text), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
