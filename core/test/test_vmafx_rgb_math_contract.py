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


def rgb_plan_file_findings(text: str) -> list[str]:
    c = code(text)
    if "vmafx_rgb_plan_init(" not in c:
        return []
    reasons: list[str] = []
    if "(void)planned" in c:
        reasons.append("vmafx_rgb_plan_init return cast to void")
    if not re.search(r"\bif\s*\(\s*!planned\b", c):
        reasons.append("missing branch on !planned")
    if "VmafxRgbPlan rgb = {0};" not in c:
        reasons.append("rgb plan not zero-initialized")
    if re.search(r"\bassert\s*\([^;]*\bplanned\b", c):
        reasons.append("assert on the recoverable plan result")
    return reasons


def unhandled_rgb_plan_callers(root: Path = ROOT) -> list[str]:
    src = root / "core/src"
    unhandled = []
    for path in sorted(src.rglob("*")):
        if path.suffix not in (".c", ".cpp"):
            continue
        text = path.read_text(encoding="utf-8")
        if rgb_plan_file_findings(text):
            unhandled.append(str(path.relative_to(root)))
    return unhandled


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
                    [
                        exe,
                        std,
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        "-pedantic",
                        f"-I{ROOT}/core/src",
                        "-x",
                        lang,
                        str(src),
                        "-o",
                        str(Path(tmp) / "t"),
                    ],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(done.returncode, 0, done.stderr)

    def test_kernels_use_the_shared_expression(self) -> None:
        text = KERNELS.read_text(encoding="utf-8")
        self.assertIn("#define VMAFX_RGB_FN __device__ __forceinline__", text)
        self.assertIn('#include "vmafx/rgb_math.h"', text)
        self.assertIn("VmafxRgbPlan rgb)", text)  # the plan by value
        self.assertIn("vmafx_rgb_value(&rgb, plane, r, g, b)", text)
        self.assertNotIn("coef", code(text))  # no second copy of the expression

    def test_rgb_plan_callers_handle_failure(self) -> None:
        unhandled = unhandled_rgb_plan_callers()
        self.assertEqual(unhandled, [])

    def test_scan_refuses_planted_unhandled_rgb_plan(self) -> None:
        valid_sample = (
            "static int fill(const VmafxFrameImport *d, const ImportLayout *layout) {\n"
            "    VmafxRgbPlan rgb = {0};\n"
            "    const bool planned = vmafx_rgb_plan_init(&rgb, layout, d->bpc, d->rgb_matrix,\n"
            "                                             d->rgb_range, d->rgb_out_range);\n"
            "    if (!planned) {\n"
            "        return -1;\n"
            "    }\n"
            "    return 0;\n"
            "}\n"
        )
        self.assertEqual(rgb_plan_file_findings(valid_sample), [])
        planted = {
            "vmafx_rgb_plan_init return cast to void": valid_sample.replace(
                "if (!planned) {\n        return -1;\n    }", "(void)planned;", 1
            ),
            "missing branch on !planned": valid_sample.replace(
                "if (!planned) {\n        return -1;\n    }", "", 1
            ),
            "rgb plan not zero-initialized": valid_sample.replace(
                "VmafxRgbPlan rgb = {0};", "VmafxRgbPlan rgb;", 1
            ),
            "assert on the recoverable plan result": valid_sample.replace(
                "    if (!planned) {", "    assert(planned);\n    if (!planned) {", 1
            ),
        }
        for what, mutated in planted.items():
            self.assertNotEqual(mutated, valid_sample, what)
            self.assertIn(what, rgb_plan_file_findings(mutated), what)


if __name__ == "__main__":
    sys.exit(unittest.main())
