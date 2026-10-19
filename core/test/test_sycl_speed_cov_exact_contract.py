#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the SpEED SYCL covariance design (T-SPEED-CHROMA-SYCL-COV-1ULP-2026-10-06).

``speed.c::compute_cov_kernel_scalar()`` adds ``(x - mean_x) * (y - mean_y)``
into one fp64 running sum in raster order, rounding every add, and
``compute_covariance_row()`` stores ``(float)(sum / (width * height))``. The
SYCL twins have no fp64 type (ADR-0220), so:

- ``sycl_speed_cov_math.h`` holds the reference's operations on values held
  in 64-bit integers (``sycl_soft_signed.h``): ``covariance_difference()``,
  ``covariance_term()``, ``covariance_chain_add()`` / ``covariance_chain()``
  and ``covariance_store()``; ``covariance_entry()`` is the whole entry in one
  function and calls the same helpers, one implementation of each operation;
- ``speed_sycl_pipeline.cpp`` runs them split across launches (ADR-2690): the
  differences and the products in parallel, stored as fp64 bit patterns, then
  one sequential chain per (channel, entry) over the stored terms in raster
  order, and the store; the operations and their order are the reference's;
- neither carries a parallel pair sum rounded once (``accumulate`` /
  ``ff_add`` / ``covariance_partial``) nor a group reduction of the terms:
  that design stored a neighbouring fp32 value on a cancelling entry (on a
  real 3840x1600 10-bit frame).

Device-free: reads the sources only. ``test_sycl_speed_cov_math`` checks the
entry against ``compute_cov_kernel_scalar()`` on the host and on a device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"

PIPELINE = FEATURE_ROOT / "sycl" / "speed_sycl_pipeline.cpp"
MATH = FEATURE_ROOT / "sycl" / "sycl_speed_cov_math.h"
REFERENCE = FEATURE_ROOT / "speed.c"
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def code(path: Path) -> str:
    return COMMENT.sub("", path.read_text(encoding="utf-8"))


def squash(text: str) -> str:
    return re.sub(r"\s+", " ", text)


class ReferenceLines(unittest.TestCase):
    """The lines the twin copies are still the reference's."""

    def test_reference_sum_is_sequential_fp64(self) -> None:
        text = squash(code(REFERENCE))
        for line in (
            "double val_x = data_x[i * stride_px + j];",
            "double val_y = data_y[i * stride_px + j];",
            "const double product = (val_x - mean_x) * (val_y - mean_y);",
            "result += product;",
            "float covariance = (float)(sums[k] / (dim->submatrix_width * dim->submatrix_height));",
        ):
            self.assertIn(line, text, f"speed.c no longer has: {line}")


class TwinDesign(unittest.TestCase):
    def test_math_header_replays_the_operations_in_order(self) -> None:
        text = squash(code(MATH))
        for line in (
            "for (uint32_t i = 0; i < height; i++)",
            "for (uint32_t j = 0; j < width; j++)",
            "const SoftSigned dx = covariance_difference(row_x[j], mx);",
            "const SoftSigned dy = covariance_difference(row_y[j], my);",
            "sum = signed_add(sum, covariance_term(dx, dy));",
            "return covariance_store(sum, static_cast<uint64_t>(width) * height);",
            # the shared helpers (ADR-2690): one implementation for A and the split chain
            "return vmaf_sycl_soft::signed_sub(vmaf_sycl_soft::signed_from_float(value), mean);",
            "return vmaf_sycl_soft::signed_mul(dx, dy);",
            "return vmaf_sycl_soft::signed_add(sum, vmaf_sycl_soft::signed_from_bits(term_bits));",
            "vmaf_sycl_soft::signed_div(sum, vmaf_sycl_soft::signed_from_exact(count));",
            "return vmaf_sycl_soft::signed_make(0u, 0, false);",
        ):
            self.assertIn(line, text, f"sycl_speed_cov_math.h lost: {line}")

    def test_math_header_has_no_fp64_type_and_no_pair_sum(self) -> None:
        text = code(MATH)
        self.assertIsNone(re.search(r"\bdouble\b", text), "fp64 type on the device")
        for name in ("two_sum", "two_prod", "ff_add", "quick_two_sum"):
            self.assertNotIn(name, text, f"{name}: a pair sum returned to the covariance")

    def test_math_header_chain_is_sequential(self) -> None:
        text = squash(code(MATH))
        for line in (
            "for (uint32_t k = 0; k < count; k++)",
            "sum = covariance_chain_add(sum, terms[static_cast<size_t>(k) * step]);",
        ):
            self.assertIn(line, text, f"sycl_speed_cov_math.h lost the sequential chain: {line}")

    def test_pipeline_runs_the_split_operations(self) -> None:
        text = squash(code(PIPELINE))
        for call in (
            "vmaf_sycl_speed_cov::covariance_difference(value, mean)",
            "vmaf_sycl_speed_cov::covariance_term(",
            "vmaf_sycl_speed_cov::covariance_chain(start, a_.terms + index, chains, count_)",
            "vmaf_sycl_speed_cov::covariance_store(sum, pixels)",
            "vmaf_sycl_speed_cov::covariance_zero()",
        ):
            self.assertIn(call, text, f"the pipeline no longer calls {call}")
        # one chain work-item per (channel, entry)
        self.assertIn("const uint32_t chains = a_.channels * kTriangle;", text)
        self.assertIn('#include "sycl_speed_cov_math.h"', PIPELINE.read_text(encoding="utf-8"))

    def test_pipeline_covariance_has_no_reduction(self) -> None:
        raw = PIPELINE.read_text(encoding="utf-8")
        start = raw.index("Covariance: compute_covariance_matrix()")
        end = raw.index("Eigenvalues: compute_eigenvalues()")
        section = COMMENT.sub("", raw[start:end])
        for name in (
            "reduce_over_group",
            "atomic_ref",
            "fetch_add",
            "group_barrier",
            "shift_group",
        ):
            self.assertNotIn(name, section, f"{name}: a parallel reduction of the covariance terms")

    def test_pipeline_has_no_fp64_type(self) -> None:
        self.assertIsNone(re.search(r"\bdouble\b", code(PIPELINE)), "fp64 type in the pipeline")

    def test_pipeline_has_no_parallel_pair_covariance(self) -> None:
        text = code(PIPELINE)
        for name in (
            "covariance_partial",
            "covariance_group",
            "covariance_group_size",
            "centred_product",
            "ff_div_to_float",
        ):
            self.assertNotIn(name, text, f"{name}: the pair-sum covariance came back")
        self.assertIsNone(
            re.search(r"\binline\s+Ff\s+accumulate\b", text), "the pair accumulate came back"
        )


if __name__ == "__main__":
    unittest.main()
