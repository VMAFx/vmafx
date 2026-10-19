#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The CUDA and HIP scale 1-3 decouple bound the gain product before narrowing it.

T-GPU-ADM-S123-GAIN-PRODUCT-NARROWING-2026-10-05. The CPU's
adm_decouple_band_s123() (integer_adm_kernels.h) forms the enhancement-gain
product `rst * gain` in double, bounds it by t (ADM_KERNEL_MIN / _MAX) and
narrows the bounded value to int32. The CUDA and HIP decouple_r_s123()
narrowed the product first: `int32_t rst = (int32_t)(...) * adm_enhn_gain_limit;`.
|o| reaches 1.45e9 at scale 1, so with the default limit of 100 the product
leaves int32 and the conversion is undefined; the devices' saturating
conversion happened to give the bounded value, so no score showed it.

Device-free: reads the two kernel headers and reports the form they had
before (planted below).
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

FEATURE = Path(__file__).resolve().parents[1] / "src" / "feature"
HEADERS = {
    "cuda": FEATURE / "cuda" / "integer_adm" / "adm_decouple_inline.cuh",
    "hip": FEATURE / "hip" / "integer_adm" / "adm_decouple_inline.hip",
}
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# The product narrowed to int32 before the bound.
NARROWED_PRODUCT = re.compile(
    r"int32_t\s+rst\s*=\s*\(int32_t\)\([^;]*\)\s*\*\s*adm_enhn_gain_limit\s*;"
)
BOUNDED_PRODUCT = re.compile(
    r"\(int32_t\)\(\s*\(rst_f\s*>\s*0\.f\)\s*\?\s*fmin\(gained,\s*\(double\)t_val\)\s*:\s*"
    r"fmax\(gained,\s*\(double\)t_val\)\s*\)"
)
GAINED = re.compile(r"const\s+double\s+gained\s*=\s*\(double\)rst_q\s*\*\s*adm_enhn_gain_limit\s*;")


def function(text: str, name: str) -> str:
    """The body of `name` in comment-free `text`, or an empty string."""
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", text)
    if match is None:
        return ""
    depth, index = 0, match.end() - 1
    for index in range(match.end() - 1, len(text)):
        depth += {"{": 1, "}": -1}.get(text[index], 0)
        if depth == 0:
            break
    return text[match.start() : index + 1]


def problems(backend: str, text: str) -> list[str]:
    """Why decouple_r_s123() of `text` does not bound the product before narrowing it."""
    body = function(COMMENT.sub(" ", text), "decouple_r_s123")
    if not body:
        return [f"{backend}: decouple_r_s123() not found"]
    found: list[str] = []
    if NARROWED_PRODUCT.search(body):
        found.append(f"{backend}: the gain product is narrowed to int32 before the bound")
    if not GAINED.search(body) or not BOUNDED_PRODUCT.search(body):
        found.append(f"{backend}: the bounded double product is not what is narrowed")
    return found


# decouple_r_s123() as both twins had it on master (571565a47).
OLD_FORM = """
__device__ __forceinline__ int32_t decouple_r_s123(int32_t oh, int32_t ov, int32_t od, int32_t th,
                                                   int32_t tv, int32_t td, int band, int angle_flag,
                                                   double adm_enhn_gain_limit)
{
    if (!angle_flag)
        adm_enhn_gain_limit = 1;
    int32_t rst = (int32_t)(((k * o_val) + 16384) >> 15) * adm_enhn_gain_limit;
    const float rst_f = ((float)k / 32768) * ((float)o_val / 64);
    if (angle_flag && (rst_f > 0.f))
        rst = min(rst, t_val);
    return rst;
}
"""


class LiveSources(unittest.TestCase):
    def test_both_twins_bound_the_product_before_narrowing(self) -> None:
        for backend, path in HEADERS.items():
            with self.subTest(backend=backend):
                self.assertEqual(problems(backend, path.read_text(encoding="utf-8")), [])


class PlantedRegressions(unittest.TestCase):
    def test_the_master_form_is_reported(self) -> None:
        found = problems("cuda", OLD_FORM)
        self.assertIn("cuda: the gain product is narrowed to int32 before the bound", found)
        self.assertIn("cuda: the bounded double product is not what is narrowed", found)

    def test_narrowing_the_product_again_is_reported(self) -> None:
        text = HEADERS["hip"].read_text(encoding="utf-8")
        mutated = text.replace(
            "const double gained = (double)rst_q * adm_enhn_gain_limit;",
            "const double gained = (double)(int32_t)(rst_q * adm_enhn_gain_limit);",
        )
        self.assertNotEqual(mutated, text)
        self.assertIn(
            "hip: the bounded double product is not what is narrowed", problems("hip", mutated)
        )


if __name__ == "__main__":
    unittest.main()
