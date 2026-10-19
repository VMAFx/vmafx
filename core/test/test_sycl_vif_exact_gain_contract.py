#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the exact gain arithmetic of vif_sycl (ADR-1432).

``integer_vif.c::vif_accumulate_pixel()`` forms a pixel's gain in fp64 and
truncates two results to integers before the log2 table::

    const double eps = 65536 * 1.0e-10;
    double g = sigma12 / (sigma1_sq + eps);
    uint32_t sv_sq = vif_sv_sq(sigma2_sq, g, sigma12);
    g = MIN(g, vif_enhn_gain_limit);
    ... (int64_t)((g * g * sigma1_sq)) ...

``vif_sv_sq()`` (``integer_vif_sv_sq.h``) is upstream's
``int32_t sv_sq = sigma2_sq - g * sigma12; sv_sq = (uint32_t)(MAX(sv_sq, 0));``
with x86's value and without the undefined conversion below INT32_MIN
(ADR-1561).

A SYCL kernel has no fp64 type (ADR-0220). ``sycl_integer_vif_math.h``
returns the same two integers from integer arithmetic and replays the
reference's fp64 operations (``sycl_soft_double.h``) for a sample the integers
do not decide. An fp32 gain, which the twin had before, puts a share of the
integers one off.

Device-free: reads the sources only. ``test_sycl_integer_vif_math`` checks the
arithmetic against the fp64 expressions on the host and on a device, and
``test_sycl_vif_parity`` checks the scores on a device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"
TEST_ROOT = ROOT / "core" / "test"

TWIN = "sycl/integer_vif_sycl.cpp"
MATH = "sycl/sycl_integer_vif_math.h"
SOFT = "sycl/sycl_soft_double.h"
CPU = "integer_vif.c"
MATH_TEST = "test_sycl_integer_vif_math.c"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
SPACE = re.compile(r"\s+")
FP64 = re.compile(r"\b(?:long\s+)?double\b")
# The reference's lines, in order. The test's reference_terms() copies them.
REFERENCE_LINES = (
    "const double eps = 65536 * 1.0e-10;",
    "double g = sigma12 / (sigma1_sq + eps);",
    "uint32_t sv_sq = vif_sv_sq(sigma2_sq, g, sigma12);",
    "g = MIN(g, vif_enhn_gain_limit);",
    "(int64_t)((g * g * sigma1_sq))",
)
# 65536 * 1.0e-10 as an fp64 significand and exponent.
EPS_MANT = "0x1b7cdfd9d7bdbbULL"
EPS_EXP = "-70"
HOST_ONLY = ("make_gain_limit",)


TERM_FIELDS = 7  # int32_t fields of struct vif_terms (ADR-1395)


def _code(source: str) -> str:
    """The source without comments and with whitespace collapsed."""
    return SPACE.sub(" ", COMMENT.sub(" ", source))


def _sources() -> dict[str, str]:
    sources = {
        name: (FEATURE_ROOT / name).read_text(encoding="utf-8") for name in (TWIN, MATH, SOFT, CPU)
    }
    sources[MATH_TEST] = (TEST_ROOT / MATH_TEST).read_text(encoding="utf-8")
    return sources


def _function_body(source: str, name: str) -> str:
    """Text of the first definition of `name` (brace-matched), or empty."""
    match = re.search(rf"\b{name}\([^;{{]*\)\s*\{{", source)
    if not match:
        return ""
    depth = 0
    for index in range(match.end() - 1, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start() : index + 1]
    return ""


def _reference_failures(sources: dict[str, str]) -> list[str]:
    """The reference still holds the lines the twin and its test mirror."""
    failures: list[str] = []
    for name, function in ((CPU, "vif_accumulate_pixel"), (MATH_TEST, "reference_terms")):
        body = _function_body(_code(sources[name]), function)
        position = 0
        for line in REFERENCE_LINES:
            found = body.find(line, position)
            if found < 0:
                failures.append(f"{name}: {function}() no longer holds `{line}` in order")
                break
            position = found
    return failures


def _math_failures(sources: dict[str, str]) -> list[str]:
    math = _code(sources[MATH])
    failures: list[str] = []
    if f"kEpsMant = {EPS_MANT};" not in math or f"kEpsExp = {EPS_EXP};" not in math:
        failures.append(f"{MATH}: kEpsMant / kEpsExp are not 65536 * 1.0e-10")
    if int(EPS_MANT.rstrip("UL"), 16) * 2.0 ** int(EPS_EXP) != 65536 * 1.0e-10:
        failures.append("the contract's own eps constant is not 65536 * 1.0e-10")
    select = _function_body(math, "gain_terms")
    for piece in (
        "if (!fast.replay) {",
        "gain_terms_replayed(sigma1_sq, sigma2_sq, sigma12, limit)",
    ):
        if piece not in select:
            failures.append(f"{MATH}: gain_terms() must replay what the integers do not decide")
            break
    replayed = _function_body(math, "gain_terms_replayed")
    for piece in ("soft_div(", "soft_mul(", "soft_sub_trunc(", "soft_trunc("):
        if piece not in replayed:
            failures.append(
                f"{MATH}: gain_terms_replayed() is not the reference's sequence ({piece})"
            )
    device = math
    for name in HOST_ONLY:
        device = device.replace(_function_body(math, name), " ")
    if FP64.search(device):
        failures.append(f"{MATH}: fp64 type outside the host-only helper (ADR-0220)")
    return failures


def _soft_failures(sources: dict[str, str]) -> list[str]:
    soft = _code(sources[SOFT])
    failures: list[str] = []
    if FP64.search(soft):
        failures.append(f"{SOFT}: fp64 type in kernel code (ADR-0220)")
    for name in (SOFT, MATH, TWIN):
        if "mul_hi" in _code(sources[name]):
            failures.append(
                f"{name}: sycl::mul_hi() on 64-bit operands returned wrong values in a kernel "
                "on an Arc A380; use u128_mul()"
            )
    return failures


def _twin_failures(sources: dict[str, str]) -> list[str]:
    twin = _code(sources[TWIN])
    failures: list[str] = []
    stats = _function_body(twin, "dev_vif_stats_log_domain")
    if "vmaf_sycl_ivif::gain_terms(" not in stats:
        failures.append(f"{TWIN}: the gain terms do not come from sycl_integer_vif_math.h")
    if re.search(r"\bfloat\b|sycl::fma|sycl::fmin", stats):
        failures.append(f"{TWIN}: fp32 arithmetic in the gain terms of dev_vif_stats_log_domain()")
    if "if (sigma12 > 0 && sigma2_sq > 0) {" not in stats:
        failures.append(f"{TWIN}: the gain terms are not guarded as integer_vif.c guards them")
    if "s->gain_limit = vmaf_sycl_ivif::make_gain_limit(s->vif_enhn_gain_limit);" not in twin:
        failures.append(f"{TWIN}: the gain limit is not converted on the host")
    terms = re.search(r"struct vif_terms \{(.*?)\};", twin)
    if not terms or "int64_t" in terms.group(1) or terms.group(1).count("int32_t") != TERM_FIELDS:
        failures.append(f"{TWIN}: the per-pixel terms must be seven int32_t (ADR-1395)")
    return failures


def _failures(sources: dict[str, str]) -> list[str]:
    return (
        _reference_failures(sources)
        + _math_failures(sources)
        + _soft_failures(sources)
        + _twin_failures(sources)
    )


class SyclVifExactGainContractTest(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _failures(sources)

    def test_live_sources_keep_the_exact_gain(self) -> None:
        self.assertEqual(_failures(_sources()), [])

    def test_fp32_gain_is_detected(self) -> None:
        failures = self._edited(
            TWIN,
            "        gain = vmaf_sycl_ivif::gain_terms(",
            "        const float g = (float)sigma12 / (float)sigma1_sq;\n"
            "        gain.sv_sq = (uint32_t)sycl::fma(-g, (float)sigma12, (float)sigma2_sq);\n"
            "        gain = other_terms(",
        )
        self.assertTrue(any("fp32 arithmetic" in failure for failure in failures), failures)
        self.assertTrue(any("do not come from" in failure for failure in failures), failures)

    def test_dropped_replay_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    if (!fast.replay) {\n        return fast.terms;\n    }\n",
            "    return fast.terms;\n",
        )
        self.assertTrue(any("must replay" in failure for failure in failures), failures)

    def test_fp64_in_the_device_math_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    const uint64_t product = (uint64_t)sigma12 * sigma12;",
            "    const uint64_t product = (uint64_t)((double)sigma12 * (double)sigma12);",
        )
        self.assertTrue(any("fp64 type outside" in failure for failure in failures), failures)

    def test_device_mul_hi_is_detected(self) -> None:
        failures = self._edited(
            SOFT,
            "    const U128 product = u128_mul(a.mant, b.mant);",
            "    const U128 product = {.hi = sycl::mul_hi(a.mant, b.mant), .lo = a.mant * b.mant};",
        )
        self.assertTrue(any("mul_hi" in failure for failure in failures), failures)

    def test_changed_eps_is_detected(self) -> None:
        failures = self._edited(MATH, EPS_MANT, "0x1b7cdfd9d7bdbaULL")
        self.assertTrue(any("kEpsMant" in failure for failure in failures), failures)

    def test_changed_reference_is_detected(self) -> None:
        failures = self._edited(
            CPU,
            "            double g = sigma12 / (sigma1_sq + eps);",
            "            double g = sigma12 / (double)sigma1_sq;",
        )
        self.assertTrue(any(CPU in failure for failure in failures), failures)

    def test_wide_pixel_terms_are_detected(self) -> None:
        failures = self._edited(
            TWIN,
            "struct vif_terms {\n    int32_t x;",
            "struct vif_terms {\n    int64_t x;",
        )
        self.assertTrue(any("seven int32_t" in failure for failure in failures), failures)


if __name__ == "__main__":
    unittest.main()
