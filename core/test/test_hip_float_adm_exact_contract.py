#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the bit-exact float_adm_hip design (ADR-1458).

``float_adm_hip`` returns the CPU extractor's values bit for bit by running
the arithmetic of the CUDA twin (ADR-1420): ``feature/float_adm_gpu_common.h``,
``adm_tools.c`` written out operation for operation. That header is pinned by
``test_cuda_float_adm_exact_contract`` and its division by
``test_float_adm_divides_contract``, both in every build. This contract pins
what the HIP twin adds to it:

- the spelling (``hip/float_adm/float_adm_hip_math.h``): the shared header's
  plain operators, which are the reference's operations under the strict FP
  list of the kernel build, and no ``__fmul_rn()`` family, which on HIP says
  nothing the flags do not;
- the kernels store every term and add each row left to right, with no wave
  or block reduction and no arithmetic constants of their own;
- the host takes the CSF weights, the reduced region, the angle threshold and
  the pooling from the reference's own routines, passes the gain limit as a
  double, adds the rows in fp32, floors the frame sums at the reference's
  1e-10 and checks the frame size before it claims a device resource.

Device-free: reads the sources only. Every planted regression below is a
construct the pre-ADR-1458 twin had. ``test_float_adm_device_math`` checks
the shared header against the CPU routines on the host,
``test_hip_float_adm_math`` the device's arithmetic against the host's value
by value, and ``test_hip_float_adm_parity`` the scores on a device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC_ROOT = ROOT / "core" / "src"

HOST = "feature/hip/float_adm_hip.c"
KERNEL = "feature/hip/float_adm/float_adm_score.hip"
SPELLING = "feature/hip/float_adm/float_adm_hip_math.h"
SHARED = "feature/float_adm_gpu_common.h"
BUILD = "meson.build"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
BLOCK_REDUCTION = re.compile(r"__shfl_\w+\s*\(|\batomicAdd\s*\(|\bwarpSize\b|__shared__")
# cos(1 degree)^2 or one of the two CSF constants written as an fp32 literal.
FP32_CONSTANT = re.compile(r"\b0\.(?:9996954\d*|0333333351|0666666701)f\b")
# A rounding macro or intrinsic of its own in the HIP spelling.
OWN_ROUNDING = re.compile(r"\bFADM_[FD](?:MUL|ADD|SUB|DIV)\b|__[fd](?:mul|add|sub|div)_rn")
SHARED_INCLUDE = '#include "feature/float_adm_gpu_common.h"'
KERNEL_INCLUDE = '#include "feature/hip/float_adm/float_adm_hip_math.h"'
POWF_IDENTITY = "#define FADM_POWF(x, p) (((p) == 1.0f) ? (x) : powf((x), (p)))"
TERM_STORE = "terms[fadm_term_index(FADM_SLOT_DEN + b, rx, ry, a.region_w, a.region_h)] ="
HOST_GAIN = ".adm_enhn_gain_limit = s->adm_enhn_gain_limit,"
HOST_COSINE = ".cos_1deg_sq = s->cos_1deg_sq,"
HOST_FLOOR = "const double numden_limit = 1e-10 * (w * h) / (1920.0 * 1080.0);"
HOST_SIZE_CHECK = 'adm_frame_size_check("float_adm_hip", w, h)'
HOST_FIRST_DEVICE_CALL = "vmaf_hip_context_new("
STRICT_FP = "hip_strict_fp_args = ['-ffp-contract=off', '-fhip-fp32-correctly-rounded-divide-sqrt']"


def _code(source: str) -> str:
    """The source with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", source)


def _sources() -> dict[str, str]:
    return {
        name: (SRC_ROOT / name).read_text(encoding="utf-8")
        for name in (HOST, KERNEL, SPELLING, SHARED, BUILD)
    }


def _function_body(code: str, name: str) -> str:
    """Text of the definition of `name` (brace-matched), or empty."""
    match = re.search(rf"\b{name}\([^;{{]*\)\s*\{{", code)
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


def _spelling_failures(sources: dict[str, str]) -> list[str]:
    spelling = sources[SPELLING]
    code = _code(spelling)
    failures: list[str] = []
    if SHARED_INCLUDE not in spelling:
        failures.append(f"{SPELLING}: the arithmetic is no longer the shared header's")
    if OWN_ROUNDING.search(code):
        failures.append(
            f"{SPELLING}: a rounding operation is respelled; the HIP kernels take the shared "
            "header's plain operators (ADR-1407)"
        )
    if POWF_IDENTITY not in spelling:
        failures.append(f"{SPELLING}: adm_p_norm = 1 goes through the device's powf()")
    if STRICT_FP not in sources[BUILD]:
        failures.append(f"{BUILD}: the HIP kernels are no longer built with the strict FP list")
    return failures


def _kernel_failures(sources: dict[str, str]) -> list[str]:
    kernel = sources[KERNEL]
    code = _code(kernel)
    failures: list[str] = []
    if KERNEL_INCLUDE not in kernel:
        failures.append(f"{KERNEL}: the kernels no longer compile the shared arithmetic")
    if FP32_CONSTANT.search(code):
        failures.append(f"{KERNEL}: a reference constant is an fp32 literal")
    if BLOCK_REDUCTION.search(code):
        failures.append(f"{KERNEL}: a per-wave or per-block reduction replaces the row sums")
    if TERM_STORE not in code:
        failures.append(f"{KERNEL}: the terms are no longer stored per sample")
    if not re.search(r"\bfadm_row_sum\(", code):
        failures.append(f"{KERNEL}: the row-sum kernel no longer calls fadm_row_sum()")
    for call in ("fadm_angle_flag(", "fadm_decouple_csf(", "fadm_threshold(", "fadm_cm_term("):
        if call not in code:
            failures.append(f"{KERNEL}: {call[:-1]}() is no longer the shared header's")
    return failures


def _host_failures(sources: dict[str, str]) -> list[str]:
    host = _code(sources[HOST])
    shared = _code(sources[SHARED])
    failures: list[str] = []
    for call in (
        "adm_csf_rfactor_s(",
        "adm_border_s(",
        "adm_decouple_cos_1deg_sq_s()",
    ):
        if call not in host:
            failures.append(f"{HOST}: {call.rstrip('()')}() is no longer the reference's routine")
    if "adm_pool_bands_s(" not in host and "adm_pool_bands_s(" not in shared:
        failures.append(f"{HOST}: adm_pool_bands_s() is no longer the reference's routine")
    if re.search(r"\bdwt_quant_step\b|\bpow\s*\(\s*10\.0|\blog10\s*\(", host):
        failures.append(f"{HOST}: the CSF weights are derived by a copy of dwt_quant_step()")
    if re.search(r"\bpowf\s*\(", host):
        failures.append(f"{HOST}: the host pools the band accumulators itself")
    if HOST_GAIN not in host or re.search(r"\(float\)\s*s->adm_enhn_gain_limit", host):
        failures.append(f"{HOST}: adm_enhn_gain_limit is narrowed before it reaches the kernel")
    if HOST_COSINE not in host:
        failures.append(f"{HOST}: cos^2 is not the reference's adm_decouple_cos_1deg_sq_s()")
    if not re.search(r"accum\[slot\] = fadm_fold_rows\(", host + " " + shared):
        failures.append(f"{HOST}: the rows are no longer added by fadm_fold_rows()")
    if re.search(r"\+=\s*\(double\)", host):
        failures.append(f"{HOST}: the host adds device partials in fp64")
    if HOST_FLOOR not in host:
        failures.append(f"{HOST}: the frame sums are not floored at the reference's 1e-10")
    init = _function_body(host, "init_fex_hip")
    check = init.find(HOST_SIZE_CHECK)
    device = init.find(HOST_FIRST_DEVICE_CALL)
    if check < 0 or device < 0 or check > device:
        failures.append(
            f"{HOST}: init does not refuse a frame below 17x17 before it claims a device resource"
        )
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return _spelling_failures(sources) + _kernel_failures(sources) + _host_failures(sources)


class FloatAdmHipExactContract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _contract_failures(sources)

    def _detects(self, failures: list[str], text: str) -> None:
        self.assertTrue(any(text in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_rounding_intrinsics_in_the_hip_spelling_are_detected(self) -> None:
        # On HIP the __fmul_rn() family is a plain operator (ADR-1407).
        sources = _sources()
        sources[SPELLING] += "\n#define FADM_FMUL(a, b) __fmul_rn((a), (b))\n"
        self._detects(_contract_failures(sources), "rounding operation is respelled")

    def test_device_powf_at_p_norm_one_is_detected(self) -> None:
        failures = self._edited(SPELLING, POWF_IDENTITY, "")
        self._detects(failures, "adm_p_norm = 1")

    def test_contraction_in_the_kernel_build_is_detected(self) -> None:
        failures = self._edited(
            BUILD, STRICT_FP, "hip_strict_fp_args = ['-fhip-fp32-correctly-rounded-divide-sqrt']"
        )
        self._detects(failures, "strict FP list")

    def test_fp32_constant_in_the_kernel_is_detected(self) -> None:
        # The pre-ADR-1458 kernels.
        sources = _sources()
        sources[KERNEL] += "\n#define FADM_ONE_BY_30 (0.0333333351f)\n"
        self._detects(_contract_failures(sources), "fp32 literal")

    def test_wave_reduction_is_detected(self) -> None:
        # The pre-ADR-1458 fadm_warp_reduce().
        sources = _sources()
        sources[KERNEL] += "\nfloat r(float v) { return v + __shfl_down(v, (int)warpSize / 2); }\n"
        self._detects(_contract_failures(sources), "per-wave or per-block")

    def test_host_double_sum_of_partials_is_detected(self) -> None:
        # The pre-ADR-1458 fadm_hip_reduce().
        sources = _sources()
        sources[HOST] += "\nstatic void f(double *t, const float *p) { t[0] += (double)p[0]; }\n"
        self._detects(_contract_failures(sources), "in fp64")

    def test_copied_quant_step_is_detected(self) -> None:
        # The pre-ADR-1458 fadm_dwt_quant_step().
        sources = _sources()
        sources[HOST] += "\nstatic float q(float t) { return (float)pow(10.0, (double)t); }\n"
        self._detects(_contract_failures(sources), "copy of dwt_quant_step")

    def test_host_pooling_of_its_own_is_detected(self) -> None:
        # The pre-ADR-1458 fadm_hip_pool_scale().
        sources = _sources()
        sources[HOST] += "\nstatic float p(float a, float i) { return powf(a, i); }\n"
        self._detects(_contract_failures(sources), "pools the band accumulators itself")

    def test_fp32_gain_limit_is_detected(self) -> None:
        # The pre-ADR-1458 `g->gain_limit = (float)s->adm_enhn_gain_limit`.
        failures = self._edited(
            HOST, HOST_GAIN, ".adm_enhn_gain_limit = (double)(float)s->adm_enhn_gain_limit,"
        )
        self._detects(failures, "narrowed before it reaches the kernel")

    def test_old_floor_is_detected(self) -> None:
        failures = self._edited(
            HOST, HOST_FLOOR, "const double numden_limit = 1e-2 * (w * h) / (1920.0 * 1080.0);"
        )
        self._detects(failures, "1e-10")

    def test_size_check_after_the_device_context_is_detected(self) -> None:
        sources = _sources()
        host = sources[HOST]
        check = (
            '    const int size_err = adm_frame_size_check("float_adm_hip", w, h);\n'
            "    if (size_err)\n        return size_err;\n"
        )
        context = "    int err = vmaf_hip_context_new(&s->ctx, fex->hip_device_index);\n"
        self.assertIn(check, host)
        self.assertIn(context, host)
        sources[HOST] = host.replace(check, "", 1).replace(context, context + check, 1)
        self._detects(_contract_failures(sources), "below 17x17")


if __name__ == "__main__":
    unittest.main()
