#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the bit-exact float_vif_cuda design (ADR-1412).

The CPU extractor (``float_vif.c`` / ``vif.c`` / ``vif_tools.c``) fixes four
things a twin has to copy to return its bits:

- the Gaussian taps are ``vif_get_filter()``'s, derived at run time in fp32;
- ``log2f`` is the polynomial ``log2f_approx()`` (``VIF_OPT_FAST_LOG2``), not
  a libm call;
- ``vif_sigma_nsq`` is a ``double``, so the two log arguments are fp64
  quotients and sums rounded to fp32 once;
- the per-pixel terms are added row by row into one fp32 accumulator, and the
  rows into another.

Device-free: reads the sources only. Every planted regression below is a
construct the pre-ADR-1412 twin had, so the contract fails on the old design
and passes on the new one. ``test_float_vif_device_math`` checks the
arithmetic of the shared header against the CPU routines on the host, and
``test_cuda_float_vif_parity`` checks the scores on a device; this contract
keeps the design from eroding on hosts without one.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"

HOST = "cuda/float_vif_cuda.c"
KERNEL = "cuda/float_vif/float_vif_score.cu"
DEVICE = "cuda/float_vif/float_vif_device.h"
CPU_OPTIONS = "vif_options.h"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# A decimal fp32 literal with at least seven fraction digits: a filter tap
# written into the source instead of taken from vif_get_filter().
TAP_LITERAL = re.compile(r"\b0\.\d{7,}f\b")
LIBM_LOG2 = re.compile(r"\blog2f?\s*\(")
BLOCK_REDUCTION = re.compile(r"__shfl_\w+\s*\(|\batomicAdd\s*\(")
FP64_NUMERATOR = (
    "const double num_den = FVIF_DADD((double)sv_sq, vif_sigma_nsq);",
    "const double num_arg = FVIF_DADD(1.0, FVIF_DDIV((double)gain_sq_sigma1, num_den));",
    "const double den_arg = FVIF_DADD(1.0, FVIF_DDIV((double)sigma1_sq, vif_sigma_nsq));",
    "if ((double)sigma1_sq < vif_sigma_nsq) {",
)
ROW_ACCUMULATION = (
    "accum_num = FVIF_FADD(accum_num, terms[at]);",
    "accum_den = FVIF_FADD(accum_den, terms[at + 1u]);",
)
HOST_ROW_SUM = (
    "fvif_sum_rows(s->rows_host[i], s->scale_h[i], &scores[2u * i], &scores[2u * i + 1u]);"
)


def _code(source: str) -> str:
    """The source with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", source)


def _sources() -> dict[str, str]:
    return {
        name: (FEATURE_ROOT / name).read_text(encoding="utf-8")
        for name in (HOST, KERNEL, DEVICE, CPU_OPTIONS)
    }


def _tap_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for name in (KERNEL, DEVICE):
        if TAP_LITERAL.search(_code(sources[name])):
            failures.append(f"{name}: a filter tap is a source literal, not vif_get_filter()'s")
    host = _code(sources[HOST])
    if not re.search(r"\bvif_get_filter\(\s*filter\s*,", host):
        failures.append(f"{HOST}: the taps no longer come from vif_get_filter()")
    if ".taps = s->taps[scale]," not in host:
        failures.append(f"{HOST}: a launch no longer hands the scale's taps to the kernel")
    return failures


def _log2_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    if not re.search(r"^#define VIF_OPT_FAST_LOG2\b", sources[CPU_OPTIONS], re.M):
        failures.append(
            f"{CPU_OPTIONS}: VIF_OPT_FAST_LOG2 is gone, so the CPU calls libm's log2f() and "
            "fvif_log2() no longer mirrors it"
        )
    for name in (KERNEL, DEVICE):
        if LIBM_LOG2.search(_code(sources[name])):
            failures.append(f"{name}: a libm log2 call replaces the reference's polynomial")
    return failures


def _type_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    device = _code(sources[DEVICE])
    for piece in FP64_NUMERATOR:
        if piece not in device:
            failures.append(f"{DEVICE}: vif_sigma_nsq no longer enters in fp64 ({piece})")
    if "double vif_sigma_nsq;" not in device:
        failures.append(f"{DEVICE}: the kernel argument vif_sigma_nsq is no longer a double")
    if ".vif_sigma_nsq = s->vif_sigma_nsq," not in _code(sources[HOST]):
        failures.append(f"{HOST}: vif_sigma_nsq is narrowed before it reaches the kernel")
    return failures


def _order_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    device = _code(sources[DEVICE])
    for piece in ROW_ACCUMULATION:
        if piece not in device:
            failures.append(f"{DEVICE}: a row is no longer one fp32 accumulation ({piece})")
    kernel = _code(sources[KERNEL])
    if BLOCK_REDUCTION.search(kernel):
        failures.append(f"{KERNEL}: a per-warp or per-block reduction replaces the row sums")
    if not re.search(r"\bfvif_row_sum\(", kernel):
        failures.append(f"{KERNEL}: the row-sum kernel no longer calls fvif_row_sum()")
    host = _code(sources[HOST])
    if HOST_ROW_SUM not in host:
        failures.append(f"{HOST}: the rows are no longer added by fvif_sum_rows()")
    if re.search(r"\+=\s*\(double\)", host):
        failures.append(f"{HOST}: the host adds device partials in fp64")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _tap_failures(sources)
        + _log2_failures(sources)
        + _type_failures(sources)
        + _order_failures(sources)
    )


class FloatVifCudaExactContract(unittest.TestCase):
    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_tap_table_in_the_kernel_is_detected(self) -> None:
        # The pre-ADR-1412 FVIF_COEFF_S3.
        sources = _sources()
        sources[KERNEL] += (
            "\n__device__ static const float FVIF_COEFF_S3[3] = "
            "{0.166378498f, 0.667243004f, 0.166378498f};\n"
        )
        self.assertTrue(any("source literal" in item for item in _contract_failures(sources)))

    def test_host_taps_not_from_the_cpu_routine_are_detected(self) -> None:
        sources = _sources()
        sources[HOST] = sources[HOST].replace(
            "vif_get_filter(filter, scale, (float)s->vif_kernelscale);",
            "memcpy(filter, FROZEN_TAPS[scale], sizeof(filter));",
            1,
        )
        self.assertTrue(
            any(
                "no longer come from vif_get_filter" in item for item in _contract_failures(sources)
            )
        )

    def test_libm_log2_is_detected(self) -> None:
        # The pre-ADR-1412 statistic.
        sources = _sources()
        sources[DEVICE] = sources[DEVICE].replace(
            "float num = fvif_log2((float)num_arg);", "float num = log2f((float)num_arg);", 1
        )
        self.assertTrue(any("libm log2" in item for item in _contract_failures(sources)))

    def test_cpu_switching_to_libm_log2_is_detected(self) -> None:
        sources = _sources()
        sources[CPU_OPTIONS] = sources[CPU_OPTIONS].replace(
            "#define VIF_OPT_FAST_LOG2", "/* #define VIF_OPT_FAST_LOG2 */", 1
        )
        self.assertTrue(any("VIF_OPT_FAST_LOG2" in item for item in _contract_failures(sources)))

    def test_fp32_sigma_nsq_is_detected(self) -> None:
        # The pre-ADR-1412 kernel took `float vif_sigma_nsq`.
        sources = _sources()
        sources[DEVICE] = sources[DEVICE].replace(
            "const double num_den = FVIF_DADD((double)sv_sq, vif_sigma_nsq);",
            "const float num_den = FVIF_FADD(sv_sq, (float)vif_sigma_nsq);",
            1,
        )
        sources[HOST] = sources[HOST].replace(
            ".vif_sigma_nsq = s->vif_sigma_nsq,", ".vif_sigma_nsq = (float)s->vif_sigma_nsq,", 1
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("no longer enters in fp64" in item for item in failures))
        self.assertTrue(any("narrowed" in item for item in failures))

    def test_warp_reduction_is_detected(self) -> None:
        # The pre-ADR-1412 fvif_warp_reduce().
        sources = _sources()
        sources[
            KERNEL
        ] += "\nfloat r(float v) { return v + __shfl_down_sync(0xffffffff, v, 16); }\n"
        self.assertTrue(
            any("per-warp or per-block" in item for item in _contract_failures(sources))
        )

    def test_host_double_sum_of_partials_is_detected(self) -> None:
        # The pre-ADR-1412 collect_fex_cuda().
        sources = _sources()
        sources[HOST] = sources[HOST].replace(
            HOST_ROW_SUM,
            "for (size_t j = 0; j < s->scale_h[i]; j++)\n"
            "            scores[2u * i] += (double)s->rows_host[i][2u * j];",
            1,
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("fvif_sum_rows" in item for item in failures))
        self.assertTrue(any("in fp64" in item for item in failures))


if __name__ == "__main__":
    unittest.main()
