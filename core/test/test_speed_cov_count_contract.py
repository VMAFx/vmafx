#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The device SpEED twins divide a covariance sum by the exact element count.

speed.c (compute_covariance_row()) divides its double sum by the exact
size_t count sub_w * sub_h. The CUDA, HIP and SYCL twins carry the sum as an
fp32 pair; they divided it by `(float)(sub_w * sub_h)`, which is the count
only up to 2^24 (above that an odd count has no fp32 value). The CUDA and HIP
twins now pass the count as an exact fp32 pair (count_ff / speed_hd_count_ff)
to a division that takes a pair divisor. The SYCL twin replays the reference's
fp64 sum and quotient in 64-bit integers (ADR-2690): it divides the soft-fp64
sum by the uint64 count widened exactly (signed_div / signed_from_exact).

test_speed_cov_count_division.c checks the arithmetic of the HIP header on the
host; this device-free test holds all three sources to the same form, and
reports the form they had before (planted below) on each of them.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE = ROOT / "core" / "src" / "feature"

# backend -> (files whose text is checked, division function, count helper)
TWINS = {
    "cuda": (("cuda/speed/speed_score.cu",), "ff_div_to_float", "count_ff"),
    "hip": (("hip/speed/speed_hip_device.h",), "speed_hd_ff_div_to_float", "speed_hd_count_ff"),
    "sycl": (
        ("sycl/sycl_soft_signed.h", "sycl/sycl_speed_cov_math.h"),
        "signed_div",
        "signed_from_exact",
    ),
}
# The SYCL covariance store before ADR-2690: the pair sum's quotient.
SYCL_PIPELINE = "sycl/speed_sycl_pipeline.cpp"
SYCL_PAIR_STORE = re.compile(r"\bff_div_to_float\s*\(")
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# The count rounded to one fp32 value, in any of the three spellings.
FLOAT_COUNT = re.compile(
    r"(?:static_cast<float>|\(float\))\s*\(\s*[\w>.-]*sub_w\s*\*\s*[\w>.-]*sub_h\s*\)"
)
FLOAT_DIVISOR = re.compile(r"\bfloat\s+divisor\b")


def code(text: str) -> str:
    """`text` with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", text)


def rounded_count_passed(division: str, body: str) -> bool:
    """Whether a call of `division` gets the count rounded to one fp32 value,
    directly or through a variable assigned from it."""
    names = {m.group(1) for m in re.finditer(r"\b(\w+)\s*=\s*" + FLOAT_COUNT.pattern, body)}
    for call in re.finditer(rf"\b{division}\s*\(([^;]*)\)\s*;", body):
        args = call.group(1)
        if FLOAT_COUNT.search(args) or any(re.search(rf"\b{n}\b", args) for n in names):
            return True
    return False


def problems(backend: str, text: str) -> list[str]:
    """Why the joined source `text` of `backend` does not divide by the exact count."""
    _, division, helper = TWINS[backend]
    body = code(text)
    found: list[str] = []
    if rounded_count_passed(division, body):
        found.append(f"{backend}: {division}() is given the fp32-rounded count")
    signature = re.search(rf"\b{division}\s*\(([^)]*)\)\s*\{{", body)
    if signature is None:
        found.append(f"{backend}: {division}() is not defined")
    elif FLOAT_DIVISOR.search(signature.group(1)):
        found.append(f"{backend}: {division}() takes a one-float divisor")
    if not re.search(rf"\b{division}\s*\([^;]*\b{helper}\s*\(", body, re.S):
        found.append(f"{backend}: the covariance store does not pass {helper}()")
    return found


def sycl_pipeline_problems(text: str) -> list[str]:
    """Why the SYCL pipeline `text` does not store the soft-fp64 quotient."""
    if SYCL_PAIR_STORE.search(code(text)):
        return ["sycl: the covariance store divides an fp32 pair (ff_div_to_float)"]
    return []


def source(backend: str) -> str:
    files, _, _ = TWINS[backend]
    return "\n".join((FEATURE / name).read_text(encoding="utf-8") for name in files)


# The master forms (571565a47) of each twin's division and covariance store.
OLD_CUDA = """
static __device__ __forceinline__ float ff_div_to_float(Ff value, float divisor)
{
    const float quotient = rn_div(value.hi, divisor);
    return quotient;
}
        const auto count = static_cast<float>(g.sub_w * g.sub_h);
        const float value = ff_div_to_float(Ff{s_hi[0], s_lo[0]}, count);
"""
OLD_HIP = """
static inline SPEED_HD float speed_hd_ff_div_to_float(SpeedHdFf value, float divisor)
{
    return value.hi / divisor;
}
    const float count = (float)(p->geometry.sub_w * p->geometry.sub_h);
    const float value = speed_hd_ff_div_to_float(sum, count);
"""
OLD_SYCL = """
inline float ff_div_to_float(Ff value, float divisor)
{
    return div_rn(value.hi, divisor);
}
        const auto count = static_cast<float>(a.sub_w * a.sub_h);
        const float value = ff_div_to_float({.hi = hi[0], .lo = lo[0]}, count);
"""


class LiveSources(unittest.TestCase):
    def test_every_twin_divides_by_the_exact_count(self) -> None:
        for backend in TWINS:
            with self.subTest(backend=backend):
                self.assertEqual(problems(backend, source(backend)), [])

    def test_the_sycl_pipeline_has_no_pair_store(self) -> None:
        text = (FEATURE / SYCL_PIPELINE).read_text(encoding="utf-8")
        self.assertEqual(sycl_pipeline_problems(text), [])


class PlantedRegressions(unittest.TestCase):
    def test_the_master_forms_are_reported(self) -> None:
        for backend, old in (("cuda", OLD_CUDA), ("hip", OLD_HIP)):
            with self.subTest(backend=backend):
                found = problems(backend, old)
                self.assertTrue(any("one-float divisor" in p for p in found), found)
                self.assertTrue(any("fp32-rounded count" in p for p in found), found)

    def test_the_sycl_pair_forms_are_reported(self) -> None:
        self.assertIn(
            "sycl: the covariance store does not pass signed_from_exact()",
            problems("sycl", OLD_SYCL),
        )
        self.assertTrue(sycl_pipeline_problems(OLD_SYCL))
        pair_count = OLD_SYCL.replace("count);", "count_ff(a.sub_w * a.sub_h));")
        self.assertTrue(sycl_pipeline_problems(pair_count))

    def test_a_rounded_count_next_to_the_pair_is_reported(self) -> None:
        mixed = source("cuda").replace(
            "count_ff(g.sub_w * g.sub_h)", "count_ff(static_cast<float>(g.sub_w * g.sub_h))"
        )
        self.assertNotEqual(mixed, source("cuda"))
        self.assertIn(
            "cuda: ff_div_to_float() is given the fp32-rounded count", problems("cuda", mixed)
        )

    def test_comments_are_ignored(self) -> None:
        commented = source("hip") + "\n/* was (float)(p->geometry.sub_w * p->geometry.sub_h) */\n"
        self.assertEqual(problems("hip", commented), [])


if __name__ == "__main__":
    unittest.main()
