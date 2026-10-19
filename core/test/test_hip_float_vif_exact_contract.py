#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the bit-exact float_vif_hip design (ADR-1444).

``float_vif_hip`` returns the CPU extractor's values bit for bit because it
runs the arithmetic of ``float_vif_gpu_common.h``, the header the CUDA twin
runs (ADR-1412): the CPU's taps, the CPU's polynomial ``log2``, the two log
arguments in fp64, and the terms added row by row. The header itself is
pinned by ``test_cuda_float_vif_exact_contract`` and compared with the CPU
routines by ``test_float_vif_device_math``. This contract pins what the HIP
twin adds to it:

- the kernels include the header with its default operators, which round once
  because every HIP kernel is built without contraction (ADR-1407), and hold
  no tap, no math-library ``log2`` and no reduction of their own;
- the host takes the taps from ``vif_get_filter()``, hands each launch its
  scale's taps and ``vif_sigma_nsq`` as a ``double``, and adds the rows with
  ``fvif_sum_rows()``.

Device-free: reads the sources only. Every planted regression below is a
construct the pre-ADR-1444 twin had. ``test_hip_float_vif_parity`` checks the
scores on a device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC_ROOT = ROOT / "core" / "src"

HOST = "feature/hip/float_vif_hip.c"
KERNEL = "feature/hip/float_vif/float_vif_score.hip"
BUILD = "meson.build"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# A decimal fp32 literal with at least seven fraction digits: a filter tap
# written into the source instead of taken from vif_get_filter().
TAP_LITERAL = re.compile(r"\b0\.\d{7,}f\b")
LIBM_LOG2 = re.compile(r"\blog2f?\s*\(")
BLOCK_REDUCTION = re.compile(r"__shfl_\w+\s*\(|\batomicAdd\s*\(|\bwarpSize\b")
# An operator macro of the shared header defined by the kernel file: the HIP
# twin relies on the defaults and on the build flags.
OPERATOR_OVERRIDE = re.compile(r"^\s*#\s*define\s+FVIF_(?:F|D)(?:MUL|ADD|SUB|DIV)\b", re.M)
SHARED_HEADER = '#include "feature/float_vif_gpu_common.h"'
STRICT_FP = "hip_strict_fp_args = ['-ffp-contract=off', '-fhip-fp32-correctly-rounded-divide-sqrt']"
HOST_ROW_SUM = (
    "fvif_sum_rows(s->rows_host[i], s->scale_h[i], &scores[2u * i], &scores[2u * i + 1u]);"
)
KERNEL_CALLS = ("fvif_pixel_statistic(", "fvif_row_sum(", "fvif_tap(")


TAP_SITES = 2  # places the host hands a scale's taps to a kernel


def _code(source: str) -> str:
    """The source with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", source)


def _sources() -> dict[str, str]:
    return {name: (SRC_ROOT / name).read_text(encoding="utf-8") for name in (HOST, KERNEL, BUILD)}


def _kernel_failures(sources: dict[str, str]) -> list[str]:
    kernel = _code(sources[KERNEL])
    failures: list[str] = []
    if SHARED_HEADER not in kernel:
        failures.append(f"{KERNEL}: the kernels no longer compile the shared arithmetic")
    for call in KERNEL_CALLS:
        if call not in kernel:
            failures.append(f"{KERNEL}: no call of {call}) from the shared header")
    if OPERATOR_OVERRIDE.search(kernel):
        failures.append(f"{KERNEL}: an operator of the shared header is redefined")
    if TAP_LITERAL.search(kernel):
        failures.append(f"{KERNEL}: a filter tap is a source literal, not vif_get_filter()'s")
    if LIBM_LOG2.search(kernel):
        failures.append(f"{KERNEL}: a math-library log2 call replaces the reference's polynomial")
    if BLOCK_REDUCTION.search(kernel):
        failures.append(f"{KERNEL}: a per-wave or per-block reduction replaces the row sums")
    return failures


def _host_failures(sources: dict[str, str]) -> list[str]:
    host = _code(sources[HOST])
    failures: list[str] = []
    if not re.search(r"\bvif_get_filter\(\s*filter\s*,", host):
        failures.append(f"{HOST}: the taps no longer come from vif_get_filter()")
    if host.count(".taps = s->taps[scale],") != TAP_SITES:
        failures.append(f"{HOST}: a launch no longer hands the scale's taps to the kernel")
    if ".vif_sigma_nsq = s->vif_sigma_nsq," not in host:
        failures.append(f"{HOST}: vif_sigma_nsq is narrowed before it reaches the kernel")
    if HOST_ROW_SUM not in host:
        failures.append(f"{HOST}: the rows are no longer added by fvif_sum_rows()")
    if re.search(r"\+=\s*\(double\)", host):
        failures.append(f"{HOST}: the host adds device partials in fp64")
    return failures


def _build_failures(sources: dict[str, str]) -> list[str]:
    build = sources[BUILD]
    failures: list[str] = []
    if STRICT_FP not in build:
        failures.append(f"{BUILD}: the HIP kernels are no longer built without contraction")
    if "'float_vif_score' : feature_src_dir + 'hip/float_vif/float_vif_score.hip'," not in build:
        failures.append(f"{BUILD}: float_vif_score.hip is not in hip_kernel_sources")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return _kernel_failures(sources) + _host_failures(sources) + _build_failures(sources)


class FloatVifHipExactContract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _contract_failures(sources)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_tap_table_in_the_kernel_is_detected(self) -> None:
        # The pre-ADR-1444 FVIF_COEFF_S3.
        sources = _sources()
        sources[KERNEL] += (
            "\n__device__ static const float FVIF_COEFF_S3[3] = "
            "{0.166378498f, 0.667243004f, 0.166378498f};\n"
        )
        self.assertTrue(any("source literal" in item for item in _contract_failures(sources)))

    def test_device_log2_is_detected(self) -> None:
        # The pre-ADR-1444 statistic.
        sources = _sources()
        sources[KERNEL] += "\nfloat den_of(float s, float n) { return log2f(1.0f + s / n); }\n"
        self.assertTrue(any("math-library log2" in item for item in _contract_failures(sources)))

    def test_wave_reduction_is_detected(self) -> None:
        # The pre-ADR-1444 fvif_warp_reduce().
        sources = _sources()
        sources[KERNEL] += "\nfloat r(float v) { return v + __shfl_down(v, 16); }\n"
        self.assertTrue(
            any("per-wave or per-block" in item for item in _contract_failures(sources))
        )

    def test_statistic_of_the_kernel_file_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "    fvif_pixel_statistic(mu1, mu2, xx, yy, xy, args.sigma_max_inv, "
            "args.vif_enhn_gain_limit,\n                         args.vif_sigma_nsq, num, den);",
            "    *num = mu1 + xx;\n    *den = mu2 + yy + xy;",
        )
        self.assertTrue(any("fvif_pixel_statistic(" in item for item in failures), failures)

    def test_redefined_operator_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            SHARED_HEADER,
            "#define FVIF_FMUL(a, b) fmaf((a), (b), 0.0f)\n" + SHARED_HEADER,
        )
        self.assertTrue(any("redefined" in item for item in failures), failures)

    def test_host_taps_not_from_the_cpu_routine_are_detected(self) -> None:
        failures = self._edited(
            HOST,
            "vif_get_filter(filter, scale, (float)s->vif_kernelscale);",
            "memcpy(filter, FROZEN_TAPS[scale], sizeof(filter));",
        )
        self.assertTrue(
            any("no longer come from vif_get_filter" in item for item in failures), failures
        )

    def test_fp32_sigma_nsq_is_detected(self) -> None:
        # The pre-ADR-1444 launch passed `(float)s->vif_sigma_nsq`.
        failures = self._edited(
            HOST, ".vif_sigma_nsq = s->vif_sigma_nsq,", ".vif_sigma_nsq = (float)s->vif_sigma_nsq,"
        )
        self.assertTrue(any("narrowed" in item for item in failures), failures)

    def test_host_double_sum_of_partials_is_detected(self) -> None:
        # The pre-ADR-1444 collect_fex_hip().
        failures = self._edited(
            HOST,
            HOST_ROW_SUM,
            "for (size_t j = 0; j < s->scale_h[i]; j++)\n"
            "            scores[2u * i] += (double)s->rows_host[i][2u * j];",
        )
        self.assertTrue(any("fvif_sum_rows" in item for item in failures), failures)
        self.assertTrue(any("in fp64" in item for item in failures), failures)

    def test_contraction_in_the_kernel_build_is_detected(self) -> None:
        failures = self._edited(
            BUILD, STRICT_FP, "hip_strict_fp_args = ['-fhip-fp32-correctly-rounded-divide-sqrt']"
        )
        self.assertTrue(any("without contraction" in item for item in failures), failures)


if __name__ == "__main__":
    unittest.main()
