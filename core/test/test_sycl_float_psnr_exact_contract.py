#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin float_psnr_sycl's sums to exact integers (ADR-1450).

``float_psnr.c`` squares each sample difference in ``float`` and adds the
squares in ``double``; that sum is exact while it is below 2^53 units of
1 / scaler^2, so the CPU's noise is the exact sum of its terms.
``float_psnr_sycl`` returns the same score exactly when its own sums are
exact. It added each 16x16 work-group in fp32, which is exact at 8 bits and
rounds at 10, 12 and 16 bits once the differences in a group are large (up to
7.4e-8 dB on full-range content).

The kernel now squares the raw sample difference in ``float`` (the CPU's
term times ``scaler``^2), converts it to an integer and the sub-groups, the
work-groups and the host add ``uint64`` values. The host divides the exact
total by ``scaler``^2 and by the pixel count.

Device-free: reads the sources only. ``test_sycl_float_psnr_parity`` compares
the scores on a device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TWIN = ROOT / "core" / "src" / "feature" / "sycl" / "float_psnr_sycl.cpp"
REFERENCE = ROOT / "core" / "src" / "feature" / "float_psnr.c"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# A floating-point accumulator anywhere in the reduction.
FLOAT_SUM = re.compile(r"\b(?:float|double) (?:my_noise|noise|subgroup_sum|total)\b")

TERM = (
    "const auto diff = (float)(r - d);",
    "const float square = diff * diff;",
    "return (uint64_t)square;",
)
ALWAYS_INLINED = "VMAF_SYCL_ALWAYS_INLINE uint64_t fpsnr_pixel_noise("
GROUP_SUM = (
    "sycl::reduce_over_group(subgroup, noise, sycl::plus<uint64_t>{});",
    "uint64_t total = 0u;",
    "partials[workgroup_index] = total;",
)
LOCAL_SUMS = "sycl::local_accessor<uint64_t, 1> const s_partials("
READBACK = "q.memcpy(s->h_partials, s->d_partials, (size_t)s->wg_count * sizeof(uint64_t));"
HOST_SUM = (
    "const double total = vmaf_float_psnr_row_noise(s->h_partials, s->height, s->wg_count_x);",
    "const double noise = (total / (scaler * scaler)) / n_pix;",
)
# The CPU's term and its sum, which the kernel and the host mirror.
REFERENCE_LINES = (
    "float diff = ref[j] - dis[j];",
    "accum += (double)(diff * diff);",
    "noise_ /= (w * h);",
)


def _flat(source: str) -> str:
    """Code without comments, every run of whitespace collapsed."""
    return " ".join(COMMENT.sub(" ", source).split())


def _sources() -> dict[str, str]:
    return {
        "twin": TWIN.read_text(encoding="utf-8"),
        "reference": REFERENCE.read_text(encoding="utf-8"),
    }


def _function_body(code: str, name: str) -> str:
    """Flattened text of the definition of `name` (brace-matched), or empty."""
    match = re.search(rf"\b{name}\([^;{{]*\) \{{", code)
    if not match:
        return ""
    depth = 0
    for index in range(match.end() - 1, len(code)):
        if code[index] == "{":
            depth += 1
        elif code[index] == "}":
            depth -= 1
            if depth == 0:
                return code[match.start() : index + 1]
    return ""


def _kernel_failures(twin: str) -> list[str]:
    code = _flat(twin)
    failures: list[str] = []
    term = _function_body(code, "fpsnr_pixel_noise")
    if any(piece not in term for piece in TERM):
        failures.append("the term is not the CPU's float product of the raw difference")
    if "inv_scaler" in term or "double" in term:
        failures.append("the term is scaled or squared in another type")
    if ALWAYS_INLINED not in code:
        failures.append("fpsnr_pixel_noise() is not always inlined into the kernel")
    group = _function_body(code, "fpsnr_store_workgroup_sum")
    kernel = _function_body(code, "launch_float_psnr")
    if any(piece not in group for piece in GROUP_SUM) or LOCAL_SUMS not in kernel:
        failures.append("a work-group sum is not an integer sum")
    if FLOAT_SUM.search(group) or FLOAT_SUM.search(kernel):
        failures.append("a work-group sum is added in floating point")
    return failures


def _host_failures(twin: str) -> list[str]:
    code = _flat(twin)
    failures: list[str] = []
    collect = _function_body(code, "collect_fex_sycl")
    if any(piece not in collect for piece in HOST_SUM):
        failures.append("the host does not add the rows' exact sums as the CPU does (ADR-1499)")
    if READBACK not in code:
        failures.append("the read-back is not one uint64 per work-group")
    rows = re.search(r"FPSNR_WG_X = 256;", code) and re.search(r"FPSNR_WG_Y = 1;", code)
    if not rows:
        failures.append("a work-group is not a segment of one row (ADR-1499)")
    return failures


def _reference_failures(reference: str) -> list[str]:
    code = _flat(reference)
    return [
        f"float_psnr.c no longer holds `{line}`; the twin mirrors it"
        for line in REFERENCE_LINES
        if line not in code
    ]


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _kernel_failures(sources["twin"])
        + _host_failures(sources["twin"])
        + _reference_failures(sources["reference"])
    )


class FloatPsnrSyclExactContract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _contract_failures(sources)

    def _assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_float_group_sum_is_detected(self) -> None:
        # The pre-ADR-1450 reduction.
        failures = self._edited(
            "twin",
            "sycl::reduce_over_group(subgroup, noise, sycl::plus<uint64_t>{});",
            "sycl::reduce_over_group(subgroup, (float)noise, sycl::plus<float>{});",
        )
        self._assert_detected(failures, "not an integer sum")

    def test_float_group_total_is_detected(self) -> None:
        failures = self._edited(
            "twin", "        uint64_t total = 0u;", "        float total = 0.0f;"
        )
        self._assert_detected(failures, "added in floating point")

    def test_frame_total_instead_of_rows_is_detected(self) -> None:
        # The pre-ADR-1499 host: every work-group added into one exact total.
        failures = self._edited(
            "twin",
            "vmaf_float_psnr_row_noise(s->h_partials, s->height, s->wg_count_x);",
            "vmaf_float_psnr_row_noise(s->h_partials, 1u, s->wg_count);",
        )
        self._assert_detected(failures, "rows' exact sums")

    def test_scaled_difference_is_detected(self) -> None:
        # The square of a scaled difference is the same value; the pin is
        # that the term stays an integer in units of 1 / scaler^2.
        failures = self._edited(
            "twin",
            "    const auto diff = (float)(r - d);",
            "    const float diff = (float)(r - d) * inv_scaler;",
        )
        self._assert_detected(failures, "float product of the raw difference")
        self._assert_detected(failures, "scaled or squared in another type")

    def test_integer_square_is_detected(self) -> None:
        # At 16 bits the CPU's float square is the integer square rounded to
        # 24 bits; an exact integer square is another number.
        failures = self._edited(
            "twin",
            "    const float square = diff * diff;",
            "    const int64_t square = (int64_t)(r - d) * (r - d);",
        )
        self._assert_detected(failures, "float product of the raw difference")

    def test_plain_inline_is_detected(self) -> None:
        # A call left in a kernel is a scratch-memory frame (ADR-1395).
        failures = self._edited(
            "twin",
            "VMAF_SYCL_ALWAYS_INLINE uint64_t fpsnr_pixel_noise(",
            "static inline uint64_t fpsnr_pixel_noise(",
        )
        self._assert_detected(failures, "always inlined")

    def test_host_division_in_another_order_is_detected(self) -> None:
        failures = self._edited(
            "twin",
            "    const double noise = (total / (scaler * scaler)) / n_pix;",
            "    const double noise = total / (scaler * scaler * n_pix);",
        )
        self._assert_detected(failures, "rows' exact sums")

    def test_narrow_readback_is_detected(self) -> None:
        failures = self._edited(
            "twin",
            "s->d_partials, (size_t)s->wg_count * sizeof(uint64_t));",
            "s->d_partials, (size_t)s->wg_count * sizeof(float));",
        )
        self._assert_detected(failures, "one uint64 per work-group")

    def test_square_groups_are_detected(self) -> None:
        # The pre-ADR-1499 geometry: 16x16 work-groups mix rows.
        failures = self._edited(
            "twin", "static constexpr int FPSNR_WG_Y = 1;", "static constexpr int FPSNR_WG_Y = 16;"
        )
        self._assert_detected(failures, "segment of one row")

    def test_changed_reference_is_detected(self) -> None:
        failures = self._edited(
            "reference",
            "accum += (double)(diff * diff);",
            "accum += (double)diff * diff;",
        )
        self._assert_detected(failures, "the twin mirrors it")


if __name__ == "__main__":
    unittest.main()
