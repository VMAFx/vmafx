#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the bit-exact float_adm_cuda design (ADR-1420).

The CPU extractor (``float_adm.c`` / ``adm.c`` / ``adm_tools.c``) fixes six
things a twin has to copy to return its bits:

- the decouple divides: the IEEE fp32 quotient on both sides since ADR-1442
  (``test_float_adm_divides_contract.py`` pins that one, for the CPU and the
  twin together);
- the angle test's threshold is ``(cos^2 * |o|^2) * |t|^2`` in that
  association, with ``cos^2`` the reference's own constant;
- the enhancement gain limit is a ``double`` and is applied in fp64;
- ``FLOAT_ONE_BY_30`` and ``FLOAT_ONE_BY_15`` are double literals, and the
  masking threshold is one nine-term sum per band with the centre tap fifth;
- every reduction is one fp32 accumulator per row and another over the rows;
- the CSF weights, the reduced region, the pooling of the band accumulators
  and the 1e-10 floor of the frame sums are the reference's.

The arithmetic is ``feature/float_adm_gpu_common.h``, which ``float_adm_hip``
runs as well (ADR-1458); ``cuda/float_adm/float_adm_device.h`` gives it the
spelling of a CUDA device, the ``__fmul_rn()`` family.

Device-free: reads the sources only. Every planted regression below is a
construct the pre-ADR-1420 twin had, so the contract fails on the old design
and passes on the new one. ``test_float_adm_device_math`` checks the
arithmetic of the shared header against the CPU routines on the host, and
``test_cuda_float_adm_parity`` checks the scores on a device; this contract
keeps the design from eroding on hosts without one.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"

HOST = "cuda/float_adm_cuda.c"
KERNEL = "cuda/float_adm/float_adm_score.cu"
# The arithmetic, shared with float_adm_hip.
DEVICE = "float_adm_gpu_common.h"
# Its spelling for CUDA device code.
SPELLING = "cuda/float_adm/float_adm_device.h"
CPU = "adm_tools.c"
CPU_OPTIONS = "adm_options.h"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
BLOCK_REDUCTION = re.compile(r"__shfl_\w+\s*\(|\batomicAdd\s*\(")
# cos(1 degree)^2 or one of the two CSF constants written as an fp32 literal.
FP32_CONSTANT = re.compile(r"\b0\.(?:9996954\d*|0333333351|0666666701)f\b")

ANGLE_THRESHOLD = "const float rhs = FADM_FMUL(FADM_FMUL(cos_1deg_sq, o_mag_sq), t_mag_sq);"
FP64_GAIN = "const double gained = FADM_DMUL((double)rst, a->adm_enhn_gain_limit);"
FP64_FILTER = "return (float)FADM_DMUL(FADM_ONE_BY_30, (double)fadm_abs(csf));"
FP64_CENTRE = (
    "sum = (float)FADM_DADD((double)sum, FADM_DMUL(FADM_ONE_BY_15, (double)fadm_abs(centre)));"
)
ROW_ACCUMULATION = "inner = FADM_FADD(inner, terms[(size_t)x * stride]);"
FRAME_ACCUMULATION = "accum = FADM_FADD(accum, rows[y]);"
HOST_FLOOR = "const double numden_limit = 1e-10 * (w * h) / (1920.0 * 1080.0);"
# Every rounding of the shared header as an explicit round-to-nearest
# intrinsic on a CUDA device.
CUDA_ROUNDING = (
    "#define FADM_FMUL(a, b) __fmul_rn((a), (b))",
    "#define FADM_FADD(a, b) __fadd_rn((a), (b))",
    "#define FADM_FSUB(a, b) __fsub_rn((a), (b))",
    "#define FADM_FDIV(a, b) __fdiv_rn((a), (b))",
    "#define FADM_DMUL(a, b) __dmul_rn((a), (b))",
    "#define FADM_DADD(a, b) __dadd_rn((a), (b))",
)
SHARED_INCLUDE = '#include "feature/float_adm_gpu_common.h"'
# The gain is applied in the positive and in the negative branch.
GAIN_BRANCHES = 2
# adm_tools.c's reductions that conclude through adm_pool_bands_s().
CPU_REDUCTIONS = 4


def _code(source: str) -> str:
    """The source with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", source)


def _sources() -> dict[str, str]:
    return {
        name: (FEATURE_ROOT / name).read_text(encoding="utf-8")
        for name in (HOST, KERNEL, DEVICE, SPELLING, CPU, CPU_OPTIONS)
    }


def _decouple_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    device = _code(sources[DEVICE])
    if ANGLE_THRESHOLD not in device:
        failures.append(f"{DEVICE}: the angle threshold is not (cos^2 * |o|^2) * |t|^2")
    if device.count(FP64_GAIN) != GAIN_BRANCHES:
        failures.append(f"{DEVICE}: the enhancement gain is no longer applied in fp64")
    if "double adm_enhn_gain_limit;" not in device:
        failures.append(f"{DEVICE}: the kernel argument adm_enhn_gain_limit is not a double")
    if re.search(r"\bf(?:min|max)f\s*\(", device):
        failures.append(f"{DEVICE}: fminf/fmaxf replace the reference's ternaries")
    host = _code(sources[HOST])
    if ".adm_enhn_gain_limit = s->adm_enhn_gain_limit," not in host:
        failures.append(f"{HOST}: adm_enhn_gain_limit is narrowed before it reaches the kernel")
    if ".cos_1deg_sq = s->cos_1deg_sq," not in host or "adm_decouple_cos_1deg_sq_s()" not in host:
        failures.append(f"{HOST}: cos^2 is not the reference's adm_decouple_cos_1deg_sq_s()")
    if not re.search(r"^#define ADM_OPT_AVOID_ATAN\b", sources[CPU_OPTIONS], re.M):
        failures.append(
            f"{CPU_OPTIONS}: ADM_OPT_AVOID_ATAN is gone, so the CPU's angle test is the atan "
            "form and fadm_angle_flag() no longer mirrors it"
        )
    return failures


def _threshold_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    device = _code(sources[DEVICE])
    for name in (KERNEL, DEVICE, HOST):
        if FP32_CONSTANT.search(_code(sources[name])):
            failures.append(f"{name}: a reference constant is an fp32 literal")
    if FP64_FILTER not in device:
        failures.append(f"{DEVICE}: the 1/30 filter product is no longer fp64")
    if FP64_CENTRE not in device:
        failures.append(f"{DEVICE}: the centre tap no longer enters as an fp64 addend")
    order = re.findall(r"sum = (?:FADM_FADD\(sum, n\[(\d)\]\)|\(float\)FADM_DADD)", device)
    if order != ["0", "1", "2", "3", "", "4", "5", "6", "7"]:
        failures.append(f"{DEVICE}: the nine threshold terms are not in the reference's order")
    if "accum = FADM_FADD(accum, fadm_thresh_band(n, centre));" not in device:
        failures.append(f"{DEVICE}: the threshold is no longer one sum per band")
    return failures


def _reduction_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    device = _code(sources[DEVICE])
    if ROW_ACCUMULATION not in device:
        failures.append(f"{DEVICE}: a row is no longer one fp32 accumulation")
    if FRAME_ACCUMULATION not in device:
        failures.append(f"{DEVICE}: the rows are no longer folded in fp32")
    kernel = _code(sources[KERNEL])
    if BLOCK_REDUCTION.search(kernel):
        failures.append(f"{KERNEL}: a per-warp or per-block reduction replaces the row sums")
    if not re.search(r"\bfadm_row_sum\(", kernel):
        failures.append(f"{KERNEL}: the row-sum kernel no longer calls fadm_row_sum()")
    host = _code(sources[HOST])
    device_common = _code(sources[DEVICE])
    if not re.search(r"accum\[slot\] = fadm_fold_rows\(", host + " " + device_common):
        failures.append(f"{HOST}: the rows are no longer added by fadm_fold_rows()")
    if re.search(r"\+=\s*\(double\)", host):
        failures.append(f"{HOST}: the host adds device partials in fp64")
    return failures


def _reference_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    host = _code(sources[HOST])
    device_common = _code(sources[DEVICE])
    for call in ("adm_csf_rfactor_s(", "adm_border_s("):
        if call not in host:
            failures.append(f"{HOST}: {call[:-1]}() is no longer the reference's routine")
    if "adm_pool_bands_s(" not in host and "adm_pool_bands_s(" not in device_common:
        failures.append(f"{HOST}: adm_pool_bands_s() is no longer the reference's routine")
    if re.search(r"\bdwt_quant_step\b|\bpow\s*\(\s*10\.0", host):
        failures.append(f"{HOST}: the CSF weights are derived by a copy of dwt_quant_step()")
    if re.search(r"\bpowf\s*\(", host):
        failures.append(f"{HOST}: the host pools the band accumulators itself")
    if HOST_FLOOR not in host:
        failures.append(f"{HOST}: the frame sums are not floored at the reference's 1e-10")
    cpu = _code(sources[CPU])
    if cpu.count("return adm_pool_bands_s(") != CPU_REDUCTIONS:
        failures.append(f"{CPU}: the four reductions no longer conclude through adm_pool_bands_s()")
    return failures


def _spelling_failures(sources: dict[str, str]) -> list[str]:
    spelling = sources[SPELLING]
    failures = [
        f"{SPELLING}: a rounding is not an explicit intrinsic on the device ({line})"
        for line in CUDA_ROUNDING
        if line not in spelling
    ]
    if SHARED_INCLUDE not in spelling:
        failures.append(f"{SPELLING}: the arithmetic is no longer the shared header's")
    if re.search(r"\bFADM_HD\s+\w+\s+\w+\(", _code(spelling)):
        failures.append(f"{SPELLING}: a copy of the arithmetic next to the shared header")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _spelling_failures(sources)
        + _decouple_failures(sources)
        + _threshold_failures(sources)
        + _reduction_failures(sources)
        + _reference_failures(sources)
    )


def _planted(name: str, old: str, new: str) -> list[str]:
    sources = _sources()
    if old not in sources[name]:
        raise AssertionError(f"{name}: planted regression anchor not found: {old}")
    sources[name] = sources[name].replace(old, new, 1)
    return _contract_failures(sources)


class FloatAdmCudaExactContract(unittest.TestCase):
    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_plain_operator_on_the_cuda_device_is_detected(self) -> None:
        failures = _planted(
            SPELLING,
            "#define FADM_FMUL(a, b) __fmul_rn((a), (b))",
            "#define FADM_FMUL(a, b) ((a) * (b))",
        )
        self.assertTrue(any("explicit intrinsic" in item for item in failures))

    def test_arithmetic_copied_into_the_cuda_header_is_detected(self) -> None:
        sources = _sources()
        sources[SPELLING] += "\nFADM_HD float fadm_divs(float n, float d) { return n / d; }\n"
        self.assertTrue(
            any("copy of the arithmetic" in item for item in _contract_failures(sources))
        )

    def test_angle_threshold_association_is_detected(self) -> None:
        # The pre-ADR-1420 `FADM_COS_1DEG_SQ * (o_mag * t_mag)`.
        failures = _planted(
            DEVICE,
            ANGLE_THRESHOLD,
            "const float rhs = FADM_FMUL(cos_1deg_sq, FADM_FMUL(o_mag_sq, t_mag_sq));",
        )
        self.assertTrue(any("angle threshold" in item for item in failures))

    def test_fp32_cosine_literal_is_detected(self) -> None:
        sources = _sources()
        sources[KERNEL] += "\n#define FADM_COS_1DEG_SQ (0.99969541789740297f)\n"
        self.assertTrue(any("fp32 literal" in item for item in _contract_failures(sources)))

    def test_fp32_gain_is_detected(self) -> None:
        # The pre-ADR-1420 kernels took `float gain_limit`.
        failures = _planted(
            DEVICE, FP64_GAIN, "const float gained = FADM_FMUL(rst, a->adm_enhn_gain_limit);"
        )
        self.assertTrue(any("applied in fp64" in item for item in failures))

    def test_fp32_csf_constants_are_detected(self) -> None:
        # The pre-ADR-1420 `FADM_ONE_BY_30 * fabsf(csf_a_val)` with an fp32 macro.
        failures = _planted(DEVICE, FP64_FILTER, "return FADM_FMUL(0.0333333351f, fadm_abs(csf));")
        self.assertTrue(any("fp32 literal" in item for item in failures))
        self.assertTrue(any("1/30 filter product" in item for item in failures))

    def test_centre_tap_last_is_detected(self) -> None:
        # The pre-ADR-1420 threshold added the neighbours first, the centres last.
        sources = _sources()
        device = sources[DEVICE]
        centre = "    " + FP64_CENTRE + "\n"
        last = "    sum = FADM_FADD(sum, n[7]);\n"
        self.assertIn(centre, device)
        sources[DEVICE] = device.replace(centre, "", 1).replace(last, last + centre, 1)
        self.assertTrue(any("reference's order" in item for item in _contract_failures(sources)))

    def test_warp_reduction_is_detected(self) -> None:
        # The pre-ADR-1420 fadm_warp_reduce().
        sources = _sources()
        sources[
            KERNEL
        ] += "\nfloat r(float v) { return v + __shfl_down_sync(0xffffffff, v, 16); }\n"
        self.assertTrue(
            any("per-warp or per-block" in item for item in _contract_failures(sources))
        )

    def test_host_double_sum_of_partials_is_detected(self) -> None:
        # The pre-ADR-1420 fadm_reduce_accum().
        sources = _sources()
        sources[HOST] += "\nstatic void f(double *t, const float *p) { t[0] += (double)p[0]; }\n"
        self.assertTrue(any("in fp64" in item for item in _contract_failures(sources)))

    def test_copied_quant_step_is_detected(self) -> None:
        # The pre-ADR-1420 fadm_dwt_quant_step().
        sources = _sources()
        sources[HOST] += "\nstatic float q(float t) { return (float)pow(10.0, (double)t); }\n"
        self.assertTrue(
            any("copy of dwt_quant_step" in item for item in _contract_failures(sources))
        )

    def test_old_floor_is_detected(self) -> None:
        # The pre-ADR-1420 numden_limit.
        failures = _planted(
            HOST, HOST_FLOOR, "const double numden_limit = 1e-2 * (w * h) / (1920.0 * 1080.0);"
        )
        self.assertTrue(any("1e-10" in item for item in failures))

    def test_cpu_leaving_the_shared_pooling_is_detected(self) -> None:
        failures = _planted(
            CPU,
            "    return adm_pool_bands_s(accum, b.right - b.left, b.bottom - b.top, "
            "adm_noise_weight, 3.0);",
            "    return accum[0] + accum[1] + accum[2];",
        )
        self.assertTrue(any("four reductions" in item for item in failures))

    def test_cpu_switching_to_the_atan_angle_test_is_detected(self) -> None:
        failures = _planted(
            CPU_OPTIONS, "#define ADM_OPT_AVOID_ATAN", "/* #define ADM_OPT_AVOID_ATAN */"
        )
        self.assertTrue(any("ADM_OPT_AVOID_ATAN" in item for item in failures))


if __name__ == "__main__":
    unittest.main()
