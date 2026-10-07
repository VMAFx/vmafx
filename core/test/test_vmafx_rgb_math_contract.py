#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Device-free contract of the RGB conversion's shared arithmetic (ADR-2146).

`core/src/vmafx/rgb_math.h` is compiled by the CPU reference, nvcc, hipcc and the
SYCL kernels. A SYCL kernel must be free of scratch memory (ADR-1395) and of fp64
(ADR-0220), and every twin must return the reference's integers.

Positive: the header compiles alone as C and as C++ with every warning an error,
and contains only the integer expression (no floating-point type, no division or
modulo, no array indexed by a run-time value). Negative: the same scan refuses a
planted `double`, a planted division and a planted run-time index. Boundary: the
device kernels pass the plan by value and include the header with the function
qualifier; nothing else defines the expression.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "core/src/vmafx/rgb_math.h"
KERNELS = ROOT / "core/src/vmafx/import_convert_kernels.h"
BANNED = (
    (r"\b(float|double|long\s+double)\b", "floating-point type"),
    (r"(?<![/*])/(?![/*])", "division"),
    (r"%", "modulo"),
    (r"\[\s*(plane|p|i|k|x|y)\s*\]", "array indexed by a run-time value"),
)


def code(text: str) -> str:
    """The text without comments and string literals."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r'"[^"\n]*"', '""', text)


def findings(text: str) -> list[str]:
    return [what for pattern, what in BANNED if re.search(pattern, code(text))]


class RgbMathContract(unittest.TestCase):
    def test_header_is_the_integer_expression_only(self) -> None:
        self.assertEqual(findings(HEADER.read_text(encoding="utf-8")), [])

    def test_scan_refuses_planted_defects(self) -> None:
        text = HEADER.read_text(encoding="utf-8")
        planted = {
            "floating-point type": text.replace("int64_t t =", "double t =", 1),
            "division": text.replace(">> VMAFX_RGB_SHIFT", "/ 1073741824", 1),
            "array indexed by a run-time value": text.replace(
                "plan->coef[1][0]", "plan->coef[plane][0]", 1
            ),
        }
        for what, mutated in planted.items():
            self.assertNotEqual(mutated, text, what)
            self.assertIn(what, findings(mutated), what)

    def test_header_compiles_alone(self) -> None:
        compilers = [("cc", "c", "-std=c11"), ("c++", "c++", "-std=c++17")]
        for tool, lang, std in compilers:
            exe = shutil.which(tool)
            if exe is None:
                continue
            with tempfile.TemporaryDirectory() as tmp:
                src = Path(tmp) / f"t.{'c' if lang == 'c' else 'cpp'}"
                src.write_text('#include "vmafx/rgb_math.h"\nint main(void) { return 0; }\n')
                done = subprocess.run(  # noqa: S603 -- resolved compiler, fixed argv
                    [exe, std, "-Wall", "-Wextra", "-Werror", "-pedantic", f"-I{ROOT}/core/src",
                     "-x", lang, str(src), "-o", str(Path(tmp) / "t")],
                    capture_output=True, text=True, check=False,
                )
                self.assertEqual(done.returncode, 0, done.stderr)

    def test_kernels_use_the_shared_expression(self) -> None:
        text = KERNELS.read_text(encoding="utf-8")
        self.assertIn("#define VMAFX_RGB_FN __device__ __forceinline__", text)
        self.assertIn('#include "vmafx/rgb_math.h"', text)
        self.assertIn("VmafxRgbPlan rgb)", text)  # the plan by value
        self.assertIn("vmafx_rgb_value(&rgb, plane, r, g, b)", text)
        self.assertNotIn("coef", code(text))  # no second copy of the expression


if __name__ == "__main__":
    sys.exit(unittest.main())
