#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the raster-order frame sums of float_ssim_cuda.

The CPU extractor (``iqa/ssim_tools.c::iqa_ssim()``) adds the term of every
window into one ``double`` per sum, left to right and top to bottom, and
returns each mean as a float. The twin's per-window terms are the CPU's bit
for bit (ADR-1399), so the only thing left that can move a score is the order
of the frame sum: a per-block sum of the same terms rounded the mean of the
frame in ``float_ssim_order_frame.h`` to the neighbouring float
(T-GPU-FLOAT-SSIM-FRAME-SUM-ORDER-2026-10-02).

``float_ssim_cuda`` therefore does not reduce the terms on the device, in the
form ADR-1424 gave ``integer_ssim_cuda``: each pass-2 kernel stores its
window's terms at the window's raster position, the host reads the plane back
and ``float_ssim_frame_sum()`` adds each sum in index order.

Device-free: reads the sources only. Every planted regression below is a
construct the earlier twin had, so the contract fails on that design and passes
on this one. ``test_cuda_float_ssim_order`` checks the bits on a device.
"""

from __future__ import annotations

import hashlib
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CUDA_ROOT = ROOT / "core" / "src" / "feature" / "cuda"

HOST = "integer_ssim_cuda.c"
KERNEL = "integer_ssim/ssim_score.cu"
FIXTURE = ROOT / "core" / "test" / "float_ssim_order_frame.h"
# The frame pair is shared by the CUDA, HIP and SYCL twin tests; the three
# lanes add the same file, so its bytes are fixed.
FIXTURE_SHA256 = "6dee502f6f583ea511b82e04441ca4e08b8aff71248026eaabe11aba7a108777"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
TERM_STORE = (
    "terms[(size_t)y * w_final + x] = ssim_terms(vertical_moments(in, x, y, w_horiz), c1, c2).ssim;"
)
LCS_STORES = (
    "(((size_t)y * w_final + x) * LCS_TERMS);",
    "window[0] = t.ssim;",
    "window[1] = t.l;",
    "window[2] = t.c;",
    "window[3] = t.s;",
)
# A shuffle of a double or a shared array of doubles: a device reduction.
DOUBLE_REDUCTION = re.compile(r"__shfl_\w+\s*\(|__shared__\s+double\b")
HOST_SUM = (
    "double ssim = 0.0;",
    "for (size_t i = 0u; i < n_windows; i++) ssim += terms[i];",
)
HOST_LCS_SUM = (
    "const double *window = terms + (i * FLOAT_SSIM_LCS_SUMS);",
    "ssim += window[0];",
    "l += window[1];",
    "c += window[2];",
    "st += window[3];",
)
HOST_CALL = "sums[0] = float_ssim_frame_sum(s->rb.host_pinned, s->n_windows);"
HOST_LCS_CALL = "float_ssim_frame_sums_lcs(s->rb.host_pinned, s->n_windows, sums);"
READBACK = "return s->n_windows * s->n_sums * sizeof(double);"
WINDOWS = "s->n_windows = (size_t)s->w_final * s->h_final;"


def _code(source: str) -> str:
    """The source with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", source)


def _flat(source: str) -> str:
    """Comment-free source on one line, so a formatter's wrapping cannot hide a statement."""
    return " ".join(_code(source).split())


def _function_body(code: str, name: str) -> str:
    """The text from the definition of `name` to the closing brace in column 0."""
    start = code.find(f" {name}(")
    if start < 0:
        return ""
    end = code.find("\n}", start)
    return code[start:end] if end >= 0 else ""


def _sources() -> dict[str, str]:
    return {name: (CUDA_ROOT / name).read_text(encoding="utf-8") for name in (HOST, KERNEL)}


def _kernel_failures(kernel: str) -> list[str]:
    failures: list[str] = []
    flat = _flat(kernel)
    if " ".join(TERM_STORE.split()) not in flat:
        failures.append(f"{KERNEL}: the SSIM term is no longer stored at its raster position")
    for piece in LCS_STORES:
        if piece not in flat:
            failures.append(f"{KERNEL}: the enable_lcs terms are not stored per window ({piece})")
    if DOUBLE_REDUCTION.search(_code(kernel)):
        failures.append(f"{KERNEL}: the double terms are reduced on the device")
    return failures


def _host_failures(host: str) -> list[str]:
    failures: list[str] = []
    flat = _flat(host)
    for piece in HOST_SUM:
        if piece not in flat:
            failures.append(
                f"{HOST}: float_ssim_frame_sum() is not one double in raster order ({piece})"
            )
    lcs_sum = " ".join(_function_body(_code(host), "float_ssim_frame_sums_lcs").split())
    for piece in HOST_LCS_SUM:
        if piece not in lcs_sum:
            failures.append(
                f"{HOST}: float_ssim_frame_sums_lcs() is not four doubles in raster order ({piece})"
            )
    if lcs_sum.count("for (") != 1 or "for (size_t i = 0u; i < n_windows; i++)" not in lcs_sum:
        failures.append(f"{HOST}: float_ssim_frame_sums_lcs() is not one pass in raster order")
    if HOST_CALL not in flat:
        failures.append(f"{HOST}: collect no longer adds the whole term plane")
    if HOST_LCS_CALL not in flat:
        failures.append(f"{HOST}: the L, C and S sums no longer add the whole term plane")
    if READBACK not in flat or WINDOWS not in flat:
        failures.append(f"{HOST}: the readback is not one double per window and sum")
    if "partials" in flat:
        failures.append(f"{HOST}: the readback holds per-block partial sums")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return _kernel_failures(sources[KERNEL]) + _host_failures(sources[HOST])


class FloatSsimCudaExactContract(unittest.TestCase):
    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_fixture_is_the_shared_frame(self) -> None:
        digest = hashlib.sha256(FIXTURE.read_bytes()).hexdigest()
        self.assertEqual(digest, FIXTURE_SHA256)

    def test_warp_reduction_of_the_term_is_detected(self) -> None:
        # The earlier kernel's block_sum().
        sources = _sources()
        sources[KERNEL] += (
            "\ndouble r(double value) {"
            " return value + __shfl_down_sync(0xffffffff, value, 16); }\n"
        )
        self.assertTrue(
            any("reduced on the device" in item for item in _contract_failures(sources))
        )

    def test_block_partial_array_is_detected(self) -> None:
        # The earlier kernel's `s_warp_sums`.
        sources = _sources()
        sources[KERNEL] += "\nvoid f(void) { __shared__ double s_warp_sums[4]; }\n"
        self.assertTrue(
            any("reduced on the device" in item for item in _contract_failures(sources))
        )

    def test_unstored_term_is_detected(self) -> None:
        sources = _sources()
        sources[KERNEL] = sources[KERNEL].replace(
            "terms[(size_t)y * w_final + x] =", "const double my_ssim =", 1
        )
        self.assertTrue(any("raster position" in item for item in _contract_failures(sources)))

    def test_unstored_lcs_term_is_detected(self) -> None:
        sources = _sources()
        sources[KERNEL] = sources[KERNEL].replace("window[3] = t.s;", "(void)t.s;", 1)
        self.assertTrue(any("enable_lcs terms" in item for item in _contract_failures(sources)))

    def test_host_sum_of_block_partials_is_detected(self) -> None:
        # The earlier collect_fex_cuda().
        sources = _sources()
        sources[HOST] = sources[HOST].replace(
            HOST_CALL, "sum_partials(s->rb.host_pinned, s->partials_count);", 1
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("whole term plane" in item for item in failures))
        self.assertTrue(any("per-block partial sums" in item for item in failures))

    def test_block_sized_readback_is_detected(self) -> None:
        sources = _sources()
        sources[HOST] = sources[HOST].replace(WINDOWS, "s->n_windows = (size_t)grid_x * grid_y;", 1)
        self.assertTrue(any("per window and sum" in item for item in _contract_failures(sources)))

    def test_reordered_host_sum_is_detected(self) -> None:
        sources = _sources()
        sources[HOST] = sources[HOST].replace(
            "for (size_t i = 0u; i < n_windows; i++)\n        ssim += terms[i];",
            "for (size_t i = n_windows; i-- > 0u;)\n        ssim += terms[i];",
            1,
        )
        self.assertTrue(any("one double in raster" in item for item in _contract_failures(sources)))

    def test_reordered_lcs_sum_is_detected(self) -> None:
        sources = _sources()
        sources[HOST] = sources[HOST].replace(
            "for (size_t i = 0u; i < n_windows; i++) {",
            "for (size_t i = n_windows; i-- > 0u;) {",
            1,
        )
        self.assertTrue(
            any("one pass in raster order" in item for item in _contract_failures(sources))
        )

    def test_lcs_sum_of_another_term_is_detected(self) -> None:
        sources = _sources()
        sources[HOST] = sources[HOST].replace("st += window[3];", "st += window[2];", 1)
        self.assertTrue(any("four doubles" in item for item in _contract_failures(sources)))

    def test_lcs_sum_of_part_of_the_plane_is_detected(self) -> None:
        sources = _sources()
        sources[HOST] = sources[HOST].replace(
            HOST_LCS_CALL,
            "float_ssim_frame_sums_lcs(s->rb.host_pinned, s->n_windows / 2u, sums);",
            1,
        )
        self.assertTrue(any("L, C and S sums" in item for item in _contract_failures(sources)))


if __name__ == "__main__":
    unittest.main()
