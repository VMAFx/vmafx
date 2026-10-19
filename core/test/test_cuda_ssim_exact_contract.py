#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the bit-exact integer_ssim_cuda design (ADR-1424).

The CPU extractor (``integer_ssim.c::calc_ssim()``) adds the SSIM term of
every pixel into one ``double``, left to right and top to bottom. The moments
are int64 and the term is one double expression, so a twin that computes both
as the CPU does still differs from it if it adds the terms in another order.

``integer_ssim_cuda`` therefore does not reduce the terms on the device: the
kernel stores one double per pixel at its raster position, the host reads the
plane back and ``issim_frame_sum()`` adds it in that order. The weights are
integers and may be reduced per block.

Device-free: reads the sources only. Every planted regression below is a
construct the pre-ADR-1424 twin had, so the contract fails on the old design
and passes on the new one. ``test_cuda_ssim_parity`` checks the scores on a
device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CUDA_ROOT = ROOT / "core" / "src" / "feature" / "cuda"

HOST = "ssim_cuda.c"
KERNEL = "integer_ssim/integer_ssim_score.cu"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
TERM_STORE = "terms[(size_t)y * width + x] = term;"
# A shuffle or a shared array of the double term: a device reduction of it.
DOUBLE_REDUCTION = re.compile(r"__shfl_\w+\s*\([^;]*(?:ssim|term)|__shared__\s+double\b")
HOST_SUM = (
    "double ssim = 0.0;",
    "for (size_t i = 0u; i < count; i++)",
    "ssim += terms[i];",
)
HOST_CALL = "issim_frame_sum(terms, (size_t)s->width * s->height);"
READBACK = "(size_t)s->width * s->height * sizeof(double)"
TERM_GROUPING = "*term = w_d * a * b / den;"


def _code(source: str) -> str:
    """The source with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", source)


def _sources() -> dict[str, str]:
    return {name: (CUDA_ROOT / name).read_text(encoding="utf-8") for name in (HOST, KERNEL)}


def _kernel_failures(kernel: str) -> list[str]:
    failures: list[str] = []
    code = _code(kernel)
    if TERM_STORE not in code:
        failures.append(f"{KERNEL}: the term is no longer stored at its raster position")
    if DOUBLE_REDUCTION.search(code):
        failures.append(f"{KERNEL}: the double terms are reduced on the device")
    if TERM_GROUPING not in code:
        failures.append(f"{KERNEL}: the term is not grouped ((w * a) * b) / den like the CPU's")
    return failures


def _host_failures(host: str) -> list[str]:
    failures: list[str] = []
    code = _code(host)
    for piece in HOST_SUM:
        if piece not in code:
            failures.append(
                f"{HOST}: issim_frame_sum() is not one double in raster order ({piece})"
            )
    if HOST_CALL not in code:
        failures.append(f"{HOST}: collect no longer adds the whole term plane")
    if READBACK not in code:
        failures.append(f"{HOST}: the readback is not one double per pixel")
    if "block_count * sizeof(double)" in code:
        failures.append(f"{HOST}: the readback holds per-block partial sums")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return _kernel_failures(sources[KERNEL]) + _host_failures(sources[HOST])


class IntegerSsimCudaExactContract(unittest.TestCase):
    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_warp_reduction_of_the_term_is_detected(self) -> None:
        # The pre-ADR-1424 kernel.
        sources = _sources()
        sources[KERNEL] += (
            "\ndouble r(double warp_ssim) {"
            " return warp_ssim + __shfl_down_sync(0xffffffffu, warp_ssim, 16); }\n"
        )
        self.assertTrue(
            any("reduced on the device" in item for item in _contract_failures(sources))
        )

    def test_block_partial_array_is_detected(self) -> None:
        # The pre-ADR-1424 `s_ssim`.
        sources = _sources()
        sources[KERNEL] += "\nvoid f(void) { __shared__ double s_ssim[4]; (void)s_ssim; }\n"
        self.assertTrue(
            any("reduced on the device" in item for item in _contract_failures(sources))
        )

    def test_unstored_term_is_detected(self) -> None:
        sources = _sources()
        sources[KERNEL] = sources[KERNEL].replace(TERM_STORE, "my_ssim = term;", 1)
        self.assertTrue(any("raster position" in item for item in _contract_failures(sources)))

    def test_regrouped_term_is_detected(self) -> None:
        sources = _sources()
        sources[KERNEL] = sources[KERNEL].replace(TERM_GROUPING, "*term = w_d * (a * b / den);", 1)
        self.assertTrue(any("grouped" in item for item in _contract_failures(sources)))

    def test_host_sum_of_block_partials_is_detected(self) -> None:
        # The pre-ADR-1424 collect_fex_cuda().
        sources = _sources()
        sources[HOST] = (
            sources[HOST]
            .replace(HOST_CALL, "issim_frame_sum(terms, (size_t)s->block_count);", 1)
            .replace(READBACK, "(size_t)s->block_count * sizeof(double)", 1)
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("whole term plane" in item for item in failures))
        self.assertTrue(any("per-block partial sums" in item for item in failures))

    def test_reordered_host_sum_is_detected(self) -> None:
        sources = _sources()
        sources[HOST] = sources[HOST].replace(
            "for (size_t i = 0u; i < count; i++)", "for (size_t i = count; i-- > 0u;)", 1
        )
        self.assertTrue(any("raster order" in item for item in _contract_failures(sources)))


if __name__ == "__main__":
    unittest.main()
