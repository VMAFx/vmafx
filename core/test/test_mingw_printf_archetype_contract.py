#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""A printf-format attribute names MinGW's archetype where MinGW defines one.

GCC on MinGW checks `format(printf, ...)` against the MSVCRT archetype, which
has no `%zu`, `%td` or `%jd`, although the UCRT64 target links the C99 printf.
`<stdio.h>` defines `__MINGW_PRINTF_FORMAT` as the archetype the runtime's own
declarations use (`gnu_printf` under UCRT, `ms_printf` under MSVCRT). A format
attribute that spells `printf` and does not consult it fails the Windows UCRT64
build with `-Werror=format` at the first `%zu` or `%td`: `error_internal.h` did
(model.c) and `compat_conformance_trace.h` did (test_compat_conformance_api.c).

The contract: every file of the C and C++ tree that writes `format(printf` also
names `__MINGW_PRINTF_FORMAT`. Device-free and compiler-free: reads the sources.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

CORE = Path(__file__).resolve().parents[1]
SCANNED = ("src", "test", "tools", "include")
SUFFIXES = {".c", ".cpp", ".h", ".hpp"}
ATTRIBUTE = re.compile(r"format\s*\(\s*printf\b")
ARCHETYPE = "__MINGW_PRINTF_FORMAT"


def offenders(files: dict[str, str]) -> list[str]:
    return sorted(
        p for p, text in files.items() if ATTRIBUTE.search(text) and ARCHETYPE not in text
    )


def tree() -> dict[str, str]:
    out: dict[str, str] = {}
    for top in SCANNED:
        for path in (CORE / top).rglob("*"):
            if path.suffix in SUFFIXES and path.is_file():
                out[path.relative_to(CORE).as_posix()] = path.read_text(
                    encoding="utf-8", errors="replace"
                )
    return out


class MingwPrintfArchetypeTest(unittest.TestCase):
    def test_every_printf_format_attribute_consults_the_mingw_archetype(self) -> None:
        files = tree()
        self.assertTrue(any(ATTRIBUTE.search(t) for t in files.values()), "no attribute found")
        self.assertEqual(offenders(files), [])

    def test_planted_plain_printf_attribute_is_refused(self) -> None:
        plain = "#define F(a, b) __attribute__((format(printf, a, b)))\n"
        guarded = "#if defined(__MINGW_PRINTF_FORMAT)\n" + plain
        self.assertEqual(offenders({"plain.h": plain}), ["plain.h"])
        self.assertEqual(offenders({"guarded.h": guarded}), [])


if __name__ == "__main__":
    unittest.main()
