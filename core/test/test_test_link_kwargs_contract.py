#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every test executable links as C++ when SYCL is on.

ADR-3357. `sycl_dependency` carries `-fsycl` and the AOT target flags in its
link arguments, which only the icpx driver accepts. With `CC=gcc CXX=icpx`
(the dev container and `Containerfile.vmafx`) a C-only test linked by GCC
fails with `unrecognized command-line option '-fsycl'`. `core/test/meson.build`
therefore defines `test_link_kwargs` once, and every `executable()` in it
passes `kwargs : test_link_kwargs` or sets `link_language` itself. This test
reads the file and fails on an executable that does neither, so a test added
later cannot quietly break the hybrid build.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

TEST_MESON = Path(__file__).resolve().parent / "meson.build"
DEFINITION = "test_link_kwargs = is_sycl_enabled ? {'link_language' : 'cpp'} : {}"
KWARGS = re.compile(r"\bkwargs\s*:\s*test_link_kwargs\b")
LINK_LANGUAGE = re.compile(r"\blink_language\s*:")
EXECUTABLE = re.compile(r"\bexecutable\s*\(")


def _strip_comments(text: str) -> str:
    # Meson comments run from `#` to the end of the line; `#` inside a string
    # literal is kept.
    out = []
    for line in text.splitlines():
        in_string = False
        cut = len(line)
        for index, char in enumerate(line):
            if char == "'":
                in_string = not in_string
            elif char == "#" and not in_string:
                cut = index
                break
        out.append(line[:cut])
    return "\n".join(out)


def _calls(code: str) -> list[tuple[int, str]]:
    """Return (line, text) of each executable() call, parentheses matched."""
    calls = []
    for match in EXECUTABLE.finditer(code):
        depth = 0
        in_string = False
        for index in range(match.end() - 1, len(code)):
            char = code[index]
            if char == "'":
                in_string = not in_string
            elif in_string:
                continue
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    line = code.count("\n", 0, match.start()) + 1
                    calls.append((line, code[match.start() : index + 1]))
                    break
        else:
            raise ValueError(f"unbalanced executable() call at offset {match.start()}")
    return calls


def missing_link_kwargs(text: str) -> list[int]:
    """Lines of executable() calls that neither pass the kwargs nor link_language."""
    return [
        line
        for line, call in _calls(_strip_comments(text))
        if not KWARGS.search(call) and not LINK_LANGUAGE.search(call)
    ]


class TestLinkKwargsContract(unittest.TestCase):
    def test_definition_is_present_once(self) -> None:
        code = _strip_comments(TEST_MESON.read_text(encoding="utf-8"))
        self.assertEqual(code.count(DEFINITION), 1)
        first = EXECUTABLE.search(code)
        assert first is not None
        self.assertLess(code.index(DEFINITION), first.start())

    def test_every_executable_links_as_cpp_with_sycl(self) -> None:
        text = TEST_MESON.read_text(encoding="utf-8")
        self.assertGreater(len(_calls(_strip_comments(text))), 0)
        self.assertEqual(missing_link_kwargs(text), [])

    def test_refuses_an_executable_without_the_kwargs(self) -> None:
        # Negative case: the check must see a planted executable.
        planted = "\n".join(
            (
                "ok = executable('ok', 'ok.c', kwargs : test_link_kwargs)",
                "own = executable('own', 'own.c', link_language : 'cpp')",
                "# executable('commented', 'c.c')",
                "bad = executable('bad', files('bad.c'),",
                "    dependencies : [dep], # kwargs : test_link_kwargs",
                ")",
            )
        )
        self.assertEqual(missing_link_kwargs(planted), [4])


if __name__ == "__main__":
    unittest.main()
