#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the bit-exact float_vif_sycl design (ADR-1422).

The CPU extractor (``float_vif.c`` / ``vif.c`` / ``vif_tools.c``) fixes four
things a twin has to copy to return its bits:

- the Gaussian taps are ``vif_get_filter()``'s, derived at run time in fp32;
- ``log2f`` is the polynomial ``log2f_approx()`` (``VIF_OPT_FAST_LOG2``), not
  a library call;
- ``vif_sigma_nsq`` is a ``double``, so the two log arguments are fp64
  quotients and sums rounded to fp32 once. A SYCL kernel has no fp64 type
  (ADR-0220): ``sycl_float_vif_math.h`` evaluates them as exact fp32 pairs
  and replays the reference's fp64 operations in integers next to a rounding
  boundary;
- the per-pixel terms are added row by row into one fp32 accumulator, and the
  rows into another.

Device-free: reads the sources only. Every planted regression below is a
construct the pre-ADR-1422 twin had or a shortcut that loses a bit, so the
contract fails on the old design and passes on the new one.
``test_sycl_float_vif_math`` checks the header's arithmetic against the CPU
routine on the host and on a device, and ``test_sycl_float_vif_parity`` checks
the scores on a device; this contract keeps the design from eroding on hosts
without one.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"

TWIN = "sycl/float_vif_sycl.cpp"
MATH = "sycl/sycl_float_vif_math.h"
CPU_OPTIONS = "vif_options.h"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# A decimal fp32 literal with at least seven fraction digits: a filter tap
# written into the source instead of taken from vif_get_filter().
TAP_LITERAL = re.compile(r"\b0\.\d{7,}f\b")
LIBRARY_LOG2 = re.compile(r"\b(?:sycl|std)::log2f?\s*\(|(?<![\w:])log2f?\s*\(")
OTHER_REDUCTION = re.compile(
    r"reduce_over_group|sycl::reduction|atomic_ref|joint_reduce|get_sub_group"
)
FP64 = re.compile(r"\b(?:long\s+)?double\b")
ROW_SUM = (
    "float numerator_sum = 0.0f;",
    "float denominator_sum = 0.0f;",
    "for (unsigned x = 0; x < args.width; x++) {",
    "numerator_sum += numerator[x];",
    "denominator_sum += denominator[x];",
)
ROW_LAUNCH = "handler.parallel_for(sycl::range<1>(args.height),"
HOST_FOLD = (
    "float numerator = 0.0f;",
    "float denominator = 0.0f;",
    "for (unsigned row = 0; row < state.scale_h[scale]; ++row) {",
    "numerator += numerator_rows[row];",
    "denominator += denominator_rows[row];",
)
STATISTIC = (
    "const float gain_den = sigma1_sq + eps;",
    "float g = sigma12 / gain_den;",
    "const float product = gain_sq * sigma1_sq;",
    ".num = log2_approx(one_plus_ratio(product, noise_plus(sv_sq, p.noise))),",
    ".den = log2_approx(one_plus_ratio(sigma1_sq, noise_denominator(p.noise))),",
    "if (sigma1_sq < p.noise.above) {",
)
REPLAY = (
    "return needs_replay(numerator, sum, denominator) ?",
    "one_plus_ratio_replayed(numerator, denominator.exact) :",
)
# The functions of the math header that are host code and use fp64.
HOST_ONLY = ("make_noise_variance", "make_statistic_params")


TAP_SITES = 2  # places the host hands a scale's taps to a kernel


def _code(source: str) -> str:
    """The source with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", source)


def _sources() -> dict[str, str]:
    return {
        name: (FEATURE_ROOT / name).read_text(encoding="utf-8")
        for name in (TWIN, MATH, CPU_OPTIONS)
    }


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


def _require(body: str, pieces: tuple[str, ...], message: str) -> list[str]:
    return [f"{message} ({piece})" for piece in pieces if piece not in body]


def _tap_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for name in (TWIN, MATH):
        if TAP_LITERAL.search(_code(sources[name])):
            failures.append(f"{name}: a filter tap is a source literal, not vif_get_filter()'s")
    twin = _code(sources[TWIN])
    init = _function_body(twin, "init_vif_taps")
    if "vif_get_filter(state.taps[scale].tap, scale, (float)state.vif_kernelscale);" not in init:
        failures.append(f"{TWIN}: the taps no longer come from vif_get_filter()")
    if twin.count(".taps = state.taps[scale],") != TAP_SITES:
        failures.append(f"{TWIN}: the filter and decimate launches must take the scale's taps")
    return failures


def _log2_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    if "#define VIF_OPT_FAST_LOG2" not in _code(sources[CPU_OPTIONS]):
        failures.append(
            f"{CPU_OPTIONS}: VIF_OPT_FAST_LOG2 is gone; the CPU's log2f is libm's now and "
            "the twin's polynomial no longer mirrors it"
        )
    for name in (TWIN, MATH):
        if LIBRARY_LOG2.search(_code(sources[name])):
            failures.append(f"{name}: a library log2 instead of log2f_approx()'s polynomial")
    return failures


def _statistic_failures(sources: dict[str, str]) -> list[str]:
    math = _code(sources[MATH])
    failures = _require(
        _function_body(math, "pixel_statistic"),
        STATISTIC,
        f"{MATH}: pixel_statistic() is not vif_pixel_statistic_s()",
    )
    failures += _require(
        _function_body(math, "one_plus_ratio"),
        REPLAY,
        f"{MATH}: one_plus_ratio() must replay the fp64 operations next to a rounding boundary",
    )
    device = math
    for name in HOST_ONLY:
        device = device.replace(_function_body(math, name), " ")
    if FP64.search(device):
        failures.append(f"{MATH}: fp64 type outside the host-only helpers (ADR-0220)")
    return failures


def _sum_failures(sources: dict[str, str]) -> list[str]:
    twin = _code(sources[TWIN])
    failures = _require(
        _function_body(twin, "vif_row_sums"),
        ROW_SUM,
        f"{TWIN}: vif_row_sums() is not the CPU's row sum",
    )
    if ROW_LAUNCH not in twin:
        failures.append(f"{TWIN}: the row sums are not launched as one work-item per row")
    if OTHER_REDUCTION.search(twin):
        failures.append(
            f"{TWIN}: a group, sub-group or atomic reduction adds the terms in an order "
            "the CPU does not use"
        )
    failures += _require(
        _function_body(twin, "sum_vif_rows"),
        HOST_FOLD,
        f"{TWIN}: sum_vif_rows() is not the CPU's fp32 sum over the rows",
    )
    return failures


def _kernel_fp64_failures(sources: dict[str, str]) -> list[str]:
    twin = _code(sources[TWIN])
    failures: list[str] = []
    for name in ("vif_row_sums", "store_vif_sigmas", "decimate_vif_pixel"):
        body = _function_body(twin, name)
        if not body:
            failures.append(f"{TWIN}: {name}() is missing")
        elif FP64.search(body):
            failures.append(f"{TWIN}: fp64 type in {name}() (ADR-0220)")
    return failures


def _option_failures(sources: dict[str, str]) -> list[str]:
    twin = _code(sources[TWIN])
    failures: list[str] = []
    for scale in (1, 2, 3):
        if f'.name = "vif_scale{scale}_min_val",' not in twin:
            failures.append(f"{TWIN}: the CPU's vif_scale{scale}_min_val option is missing")
    if ".use_minimums = true," not in _function_body(twin, "emit_vif_scores"):
        failures.append(f"{TWIN}: the per-scale floors are not applied")
    return failures


def _failures(sources: dict[str, str]) -> list[str]:
    return (
        _tap_failures(sources)
        + _log2_failures(sources)
        + _statistic_failures(sources)
        + _sum_failures(sources)
        + _kernel_fp64_failures(sources)
        + _option_failures(sources)
    )


class SyclFloatVifExactContractTest(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _failures(sources)

    def test_live_sources_keep_the_exact_design(self) -> None:
        self.assertEqual(_failures(_sources()), [])

    def test_tap_literal_is_detected(self) -> None:
        failures = self._edited(
            TWIN,
            "        const float coefficient = taps.tap[tap];",
            "        const float coefficient = tap == 0 ? 0.00745626912f : taps.tap[tap];",
        )
        self.assertTrue(any("source literal" in failure for failure in failures), failures)

    def test_taps_not_from_vif_get_filter_are_detected(self) -> None:
        failures = self._edited(
            TWIN,
            "vif_get_filter(state.taps[scale].tap, scale, (float)state.vif_kernelscale);",
            "fill_table(state.taps[scale].tap, scale);",
        )
        self.assertTrue(any("vif_get_filter()" in failure for failure in failures), failures)

    def test_device_log2_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            ".num = log2_approx(one_plus_ratio(product, noise_plus(sv_sq, p.noise))),",
            ".num = sycl::log2(one_plus_ratio(product, noise_plus(sv_sq, p.noise))),",
        )
        self.assertTrue(any("library log2" in failure for failure in failures), failures)

    def test_fp32_noise_variance_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            ".den = log2_approx(one_plus_ratio(sigma1_sq, noise_denominator(p.noise))),",
            ".den = log2_approx(1.0f + sigma1_sq / p.noise.hi),",
        )
        self.assertTrue(any("vif_pixel_statistic_s()" in failure for failure in failures), failures)

    def test_dropped_replay_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    return needs_replay(numerator, sum, denominator) ?\n"
            "               one_plus_ratio_replayed(numerator, denominator.exact) :\n"
            "               sum.hi;",
            "    return sum.hi;",
        )
        self.assertTrue(any("rounding boundary" in failure for failure in failures), failures)

    def test_fp64_in_the_device_statistic_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    const float gain_sq = g * g;",
            "    const float gain_sq = (float)((double)g * (double)g);",
        )
        self.assertTrue(any("fp64 type outside" in failure for failure in failures), failures)

    def test_sub_group_reduction_is_detected(self) -> None:
        failures = self._edited(
            TWIN,
            "    args.sigma12[index] = sigmas.sigma12;",
            "    args.sigma12[index] = sycl::reduce_over_group(\n"
            "        item.get_sub_group(), sigmas.sigma12, sycl::plus<float>{});",
        )
        self.assertTrue(any("order the CPU does not use" in failure for failure in failures))

    def test_strided_row_sum_is_detected(self) -> None:
        failures = self._edited(
            TWIN,
            "    for (unsigned x = 0; x < args.width; x++) {\n        numerator_sum += numerator[x];",
            "    for (unsigned x = 0; x < args.width; x += 2) {\n"
            "        numerator_sum += numerator[x] + numerator[x + 1];",
        )
        self.assertTrue(any("row sum" in failure for failure in failures), failures)

    def test_blocked_row_launch_is_detected(self) -> None:
        failures = self._edited(
            TWIN,
            "handler.parallel_for(sycl::range<1>(args.height),",
            "handler.parallel_for(sycl::range<2>(args.height, 4),",
        )
        self.assertTrue(any("one work-item per row" in failure for failure in failures), failures)

    def test_fp64_host_fold_is_detected(self) -> None:
        failures = self._edited(
            TWIN,
            "        float numerator = 0.0f;\n        float denominator = 0.0f;",
            "        double numerator = 0.0;\n        double denominator = 0.0;",
        )
        self.assertTrue(any("sum over the rows" in failure for failure in failures), failures)

    def test_missing_scale_floor_is_detected(self) -> None:
        failures = self._edited(TWIN, '.name = "vif_scale2_min_val",', '.name = "vif_s2",')
        self.assertTrue(any("vif_scale2_min_val" in failure for failure in failures), failures)


if __name__ == "__main__":
    unittest.main()
