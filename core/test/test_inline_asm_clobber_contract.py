#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the clobber lists of the library's inline assembly.

A GNU inline-assembly statement tells the compiler which registers it writes
through its clobber list. clang drops the clobber of a register the enclosing
function's target does not have: in a function compiled without AVX-512,
``"zmm0"`` alone clobbers nothing, and a value the caller keeps in ``xmm0``
across the statement is overwritten. GCC maps ``zmm0`` to the same hard
register as ``xmm0`` and is not affected.

``vmaf_init_cpu()``'s AVX-512 warm-up had that list. A link-time-optimised
clang build inlined it into its caller, and on a host with AVX-512
``test_ciede_device_math`` computed ``45 - 20 * log10(x)`` as ``45``.

The rule: a statement that names a ``ymm`` or ``zmm`` register in its clobber
list names the ``xmm`` register of the same number too, which every x86-64
target has.

Device-free and compiler-free: reads the sources only. ``test_cpu`` runs the
warm-up with a value in ``xmm0`` on a host that has AVX-512.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE_ROOT = ROOT / "core" / "src"
SUFFIXES = (".c", ".h", ".cpp", ".hpp", ".cc", ".cu", ".hip")
WARM_UP = "x86/avx512_warm_up.h"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# An inline-assembly statement, up to the semicolon that ends it.
ASM_STATEMENT = re.compile(r"\b(?:__asm__|__asm|asm)\b(?:\s+volatile)?\s*\((.*?)\)\s*;", re.S)
STRING = re.compile(r'"((?:[^"\\]|\\.)*)"')
WIDE_REGISTER = re.compile(r"^[yz]mm(\d+)$")


CLOBBER_SECTION = 3  # index of the clobber list: what follows the third colon


def _clobbers(statement: str) -> list[str]:
    """The clobber list of an extended asm statement: what follows the third colon."""
    template_end = 0
    for match in STRING.finditer(statement):
        if statement[template_end : match.start()].strip():
            break
        template_end = match.end()
    sections = statement[template_end:].split(":")
    if len(sections) <= CLOBBER_SECTION:
        return []
    return [match.group(1) for match in STRING.finditer(sections[CLOBBER_SECTION])]


def _failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for name, source in sorted(sources.items()):
        for statement in ASM_STATEMENT.finditer(COMMENT.sub(" ", source)):
            clobbers = _clobbers(statement.group(1))
            for register in clobbers:
                wide = WIDE_REGISTER.match(register)
                if wide and f"xmm{wide.group(1)}" not in clobbers:
                    failures.append(
                        f"{name}: an inline-assembly statement clobbers {register} without "
                        f"xmm{wide.group(1)}; clang drops the wide register's clobber in a "
                        "function not compiled for it"
                    )
    return failures


def _sources() -> dict[str, str]:
    return {
        path.relative_to(SOURCE_ROOT).as_posix(): path.read_text(encoding="utf-8", errors="replace")
        for path in sorted(SOURCE_ROOT.rglob("*"))
        if path.suffix in SUFFIXES and path.is_file()
    }


class InlineAsmClobberContract(unittest.TestCase):
    def test_live_sources_name_the_xmm_register(self) -> None:
        self.assertEqual(_failures(_sources()), [])

    def test_the_warm_up_is_still_inline_assembly(self) -> None:
        """The contract has a subject: the statement it was written for."""
        source = COMMENT.sub(" ", _sources()[WARM_UP])
        statements = [_clobbers(match.group(1)) for match in ASM_STATEMENT.finditer(source)]
        self.assertIn(["xmm0", "zmm0"], statements)

    def test_zmm_only_clobber_is_detected(self) -> None:
        # The list vmaf_init_cpu() had.
        sources = _sources()
        old = '::: "xmm0", "zmm0");'
        self.assertIn(old, sources[WARM_UP])
        sources[WARM_UP] = sources[WARM_UP].replace(old, '::: "zmm0");', 1)
        failures = _failures(sources)
        self.assertTrue(any("clobbers zmm0 without xmm0" in item for item in failures), failures)

    def test_ymm_clobber_with_another_xmm_is_detected(self) -> None:
        sources = {
            "probe.c": 'void f(void) { __asm__("vpxor %%ymm3, %%ymm3, %%ymm3" : : : "xmm0", "ymm3"); }'
        }
        failures = _failures(sources)
        self.assertTrue(any("clobbers ymm3 without xmm3" in item for item in failures), failures)

    def test_commented_statement_is_ignored(self) -> None:
        sources = {"probe.c": '/* __asm__ volatile("vpxord %%zmm0, %%zmm0, %%zmm0" ::: "zmm0"); */'}
        self.assertEqual(_failures(sources), [])

    def test_colon_inside_the_template_is_not_a_section(self) -> None:
        sources = {
            "probe.c": 'void f(void) { __asm__("1: vpxord %%zmm1, %%zmm1, %%zmm1\\n" "jmp 1b" ::: "zmm1"); }'
        }
        failures = _failures(sources)
        self.assertTrue(any("clobbers zmm1 without xmm1" in item for item in failures), failures)


if __name__ == "__main__":
    unittest.main()
