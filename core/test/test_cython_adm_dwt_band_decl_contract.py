#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The Cython extension declares ``init_dwt_band_d()`` as ``adm.c`` defines it.

``compat/python-vmaf/core/adm_dwt2_cy.pyx`` text-includes ``core/src/feature/adm.c``
and re-declares the static helper ``init_dwt_band_d()`` for Cython. When #1859
changed the helper's cursor from ``char *`` to ``double *`` (and its length from
bytes to samples), the stale declaration generated a call that no C compiler
accepts (incompatible pointer types), and every hosted job that builds the
wheel (Ubuntu clang, Ubuntu ARM clang, macOS clang) failed.

Device-free and compiler-free: the parameter types of the declaration and of
the definition must be the same, and the caller must hand the helper a length
in samples. The planted regression is the declaration this rule replaced.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PYX = ROOT / "compat" / "python-vmaf" / "core" / "adm_dwt2_cy.pyx"
ADM_C = ROOT / "core" / "src" / "feature" / "adm.c"
NAME = "init_dwt_band_d"


def param_types(signature: str) -> list[str]:
    """Return the parameter types of a C-like signature, names dropped."""
    params = signature.split(",")
    types = []
    for param in params:
        words = re.sub(r"\s*\*\s*", " * ", param.strip()).split()
        if words and words[-1] != "*":
            words = words[:-1]
        types.append(" ".join(words))
    return types


def return_and_params(text: str, prefix: str) -> tuple[str, list[str]]:
    """Find ``<ret> init_dwt_band_d(<params>)`` in ``text``."""
    match = re.search(prefix + r"([\w ]+\*?)\s*" + NAME + r"\(([^)]*)\)", text)
    if match is None:
        raise AssertionError(f"{NAME} not found in text")
    ret = re.sub(r"\s*\*\s*", " * ", match.group(1)).strip()
    return ret, param_types(match.group(2))


class CythonBandDeclaration(unittest.TestCase):
    def test_declaration_matches_definition(self) -> None:
        defined = return_and_params(ADM_C.read_text(encoding="utf-8"), r"static\s+")
        declared = return_and_params(PYX.read_text(encoding="utf-8"), r"\n\s+")
        self.assertEqual(declared, defined)

    def test_cursor_is_a_double_pointer(self) -> None:
        text = PYX.read_text(encoding="utf-8")
        self.assertRegex(text, r"cdef double \*data_top\b")
        self.assertNotRegex(text, r"data_top\s*=\s*<char \*>")

    def test_length_is_in_samples(self) -> None:
        text = PYX.read_text(encoding="utf-8")
        self.assertRegex(text, NAME + r"\(&aa_dwt2, data_top, buf_sz_one // sizeof\(double\)\)")

    def test_planted_regression_is_rejected(self) -> None:
        stale = 'cdef extern from "x":\n    char *init_dwt_band_d(adm_dwt_band_t_d *band, char *data_top, size_t buf_sz_one)\n'
        defined = return_and_params(ADM_C.read_text(encoding="utf-8"), r"static\s+")
        self.assertNotEqual(return_and_params(stale, r"\n\s+"), defined)


if __name__ == "__main__":
    unittest.main()
