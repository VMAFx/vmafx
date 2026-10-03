#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the exact gain arithmetic of integer_vif_metal (ADR-1432, ADR-1498).

``integer_vif.c::vif_accumulate_pixel()`` forms a pixel's gain in fp64 and
truncates two results to integers before the log2 table::

    const double eps = 65536 * 1.0e-10;
    double g = sigma12 / (sigma1_sq + eps);
    int32_t sv_sq = sigma2_sq - g * sigma12;
    sv_sq = (uint32_t)(MAX(sv_sq, 0));
    g = MIN(g, vif_enhn_gain_limit);
    ... (int64_t)((g * g * sigma1_sq)) ...

Metal has no fp64 type. ``metal_integer_vif_gain.h`` returns the same two
integers from integer arithmetic and replays the reference's fp64 operations
(``metal_soft_double.h``) for a sample the integers do not decide, as the SYCL
twin does (ADR-1432). An fp32 gain and an fp32 clamp, which the twin had
before, put a share of the integers one off.

Device-free: reads the sources only. ``test_metal_integer_vif_gain`` checks
the arithmetic against the fp64 expressions on the host, and
``test_metal_integer_vif_parity`` checks the scores on a device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"
TEST_ROOT = ROOT / "core" / "test"

KERNEL = "metal/integer_vif.metal"
HOST = "metal/integer_vif_metal.mm"
MATH = "metal/metal_integer_vif_gain.h"
SOFT = "metal/metal_soft_double.h"
CPU = "integer_vif.c"
MATH_TEST = "test_metal_integer_vif_gain.c"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
SPACE = re.compile(r"\s+")
FP64 = re.compile(r"\b(?:long\s+)?double\b")
LONG_LONG = re.compile(r"\blong\s+long\b|\b\d+[uU]?[lL][lL]\b|\b0x[0-9a-fA-F]+[uU]?[lL][lL]\b")
DESIGNATED = re.compile(r"[{,]\s*\.\w+\s*=")
# The reference's lines, in order. The test's reference_terms() copies them.
REFERENCE_LINES = (
    "const double eps = 65536 * 1.0e-10;",
    "double g = sigma12 / (sigma1_sq + eps);",
    "int32_t sv_sq = sigma2_sq - g * sigma12;",
    "sv_sq = (uint32_t)(MAX(sv_sq, 0));",
    "g = MIN(g, vif_enhn_gain_limit);",
    "(int64_t)((g * g * sigma1_sq))",
)
# The host tail of the CPU (vif_store_residuals()), which the twin repeats.
TAIL_LINES = (
    "(float)(num_log / 2048.0 + (den_non_log - ((double)num_non_log / 16384.0) / 65025.0))",
    "(float)(den_log / 2048.0 + (double)den_non_log)",
)
# The same two lines in the CPU's own spelling.
CPU_TAIL_LINES = (
    "num[0] = acc->accum_num_log / 2048.0 + (acc->accum_den_non_log - "
    "((acc->accum_num_non_log) / 16384.0) / (65025.0));",
    "den[0] = acc->accum_den_log / 2048.0 + acc->accum_den_non_log;",
)
# 65536 * 1.0e-10 as an fp64 significand and exponent.
EPS_MANT = "0x1b7cdfd9d7bdbb"
EPS_EXP = "-70"
HOST_GUARD = "#if !defined(__METAL_VERSION__)"


def _code(source: str) -> str:
    """The source without comments and with whitespace collapsed."""
    return SPACE.sub(" ", COMMENT.sub(" ", source))


def _sources() -> dict[str, str]:
    sources = {
        name: (FEATURE_ROOT / name).read_text(encoding="utf-8")
        for name in (KERNEL, HOST, MATH, SOFT, CPU)
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
    residuals = _function_body(_code(sources[CPU]), "vif_store_residuals")
    for line in CPU_TAIL_LINES:
        if line not in residuals:
            failures.append(f"{CPU}: vif_store_residuals() changed: `{line}`")
    return failures


def _math_failures(sources: dict[str, str]) -> list[str]:
    math = _code(sources[MATH])
    failures: list[str] = []
    if f"VMAF_MTL_IVIF_EPS_MANT VMAF_MTL_U64({EPS_MANT})" not in math or (
        f"VMAF_MTL_IVIF_EPS_EXP ({EPS_EXP})" not in math
    ):
        failures.append(f"{MATH}: the eps constants are not 65536 * 1.0e-10")
    if 65536 * 1.0e-10 != int(EPS_MANT, 16) * 2.0 ** int(EPS_EXP):
        failures.append("the contract's own eps constant is not 65536 * 1.0e-10")
    select = _function_body(math, "vmaf_mtl_ivif_gain_terms")
    for piece in (
        "if (!quick.replay) {",
        "vmaf_mtl_ivif_gain_terms_replayed(sigma1_sq, sigma2_sq, sigma12, limit)",
    ):
        if piece not in select:
            failures.append(f"{MATH}: gain_terms() must replay what the integers do not decide")
            break
    replayed = _function_body(math, "vmaf_mtl_ivif_gain_terms_replayed")
    for piece in (
        "vmaf_mtl_soft_div(",
        "vmaf_mtl_soft_mul(",
        "vmaf_mtl_soft_sub_trunc(",
        "vmaf_mtl_soft_trunc(",
    ):
        if piece not in replayed:
            failures.append(f"{MATH}: gain_terms_replayed() is not the reference's sequence ({piece})")
    if "return quick.terms;" not in select or "replay" not in _function_body(
        math, "vmaf_mtl_ivif_gain_terms_integer"
    ):
        failures.append(f"{MATH}: the integer evaluation does not report what it cannot decide")
    # The only fp64 type is in the host-only helper, behind the host guard.
    head, _, host = math.partition(HOST_GUARD)
    if FP64.search(head):
        failures.append(f"{MATH}: fp64 type outside the host-only helper (Metal has none)")
    if HOST_GUARD not in math or "vmaf_mtl_ivif_make_gain_limit" not in host:
        failures.append(f"{MATH}: make_gain_limit() must sit behind {HOST_GUARD}")
    if re.search(r"\b(half|fast|precise)\b", head):
        failures.append(f"{MATH}: an identifier that is a Metal type or namespace name")
    if LONG_LONG.search(math) or "std::" in math:
        failures.append(f"{MATH}: `long long`, a 64-bit literal suffix or std:: (not in MSL)")
    if DESIGNATED.search(head):
        failures.append(f"{MATH}: designated initializer (not in MSL)")
    return failures


def _soft_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for name in (SOFT, MATH, KERNEL):
        code = _code(sources[name])
        if re.search(r"mul_hi|mulhi", code):
            failures.append(
                f"{name}: a 64-bit multiply-high returned wrong values on one GPU; write the "
                "product in 32-bit limbs (vmaf_mtl_u128_mul)"
            )
    return failures


def _kernel_failures(sources: dict[str, str]) -> list[str]:
    kernel = _code(sources[KERNEL])
    failures: list[str] = []
    if '#include "metal_integer_vif_gain.h"' not in kernel:
        failures.append(f"{KERNEL}: the kernel does not include metal_integer_vif_gain.h")
    stat = _function_body(kernel, "ivif_pixel_stat")
    if "vmaf_mtl_ivif_gain_terms(" not in stat:
        failures.append(f"{KERNEL}: the gain terms do not come from metal_integer_vif_gain.h")
    if re.search(r"\bfloat\b|\bfma\(|\bmin\(g|\bhalf\b", stat):
        failures.append(f"{KERNEL}: fp32 arithmetic in the gain terms of ivif_pixel_stat()")
    if "if (sigma12 > 0 && sigma2_sq > 0) {" not in stat:
        failures.append(f"{KERNEL}: the gain terms are not guarded as integer_vif.c guards them")
    if FP64.search(kernel):
        failures.append(f"{KERNEL}: fp64 type in a kernel (Metal has none)")
    if "float vif_enhn_gain_limit" in kernel or "constant float4 &cfgf" in kernel:
        failures.append(f"{KERNEL}: the gain limit reaches the kernel as an fp32 value")
    if kernel.count("constant VmafMtlGainLimit &gain_limit") != 2:
        failures.append(f"{KERNEL}: both compute kernels must take a VmafMtlGainLimit")
    return failures


def _host_failures(sources: dict[str, str]) -> list[str]:
    host = _code(sources[HOST])
    failures: list[str] = []
    if "vmaf_mtl_ivif_make_gain_limit(s->vif_enhn_gain_limit)" not in host:
        failures.append(f"{HOST}: the gain limit is not converted on the host")
    if "(float)s->vif_enhn_gain_limit" in host:
        failures.append(f"{HOST}: the gain limit is narrowed to fp32 on the host")
    if host.count("setBytes:&egl length:sizeof(egl)") != 2:
        failures.append(f"{HOST}: both compute encoders must bind the VmafMtlGainLimit")
    if '#include "metal_integer_vif_gain.h"' not in host:
        failures.append(f"{HOST}: the host does not include metal_integer_vif_gain.h")
    scale = _function_body(host, "scale_num_den")
    for line in TAIL_LINES:
        if SPACE.sub("", line) not in SPACE.sub("", scale):
            failures.append(f"{HOST}: the host tail is not vif_store_residuals()'s: `{line}`")
    return failures


def _failures(sources: dict[str, str]) -> list[str]:
    return (
        _reference_failures(sources)
        + _math_failures(sources)
        + _soft_failures(sources)
        + _kernel_failures(sources)
        + _host_failures(sources)
    )


class MetalIntegerVifGainContractTest(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _failures(sources)

    def _assert_detected(self, failures: list[str], text: str) -> None:
        self.assertTrue(any(text in failure for failure in failures), failures)

    def test_live_sources_keep_the_exact_gain(self) -> None:
        self.assertEqual(_failures(_sources()), [])

    def test_fp32_gain_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "const VmafMtlGainTerms gain =",
            "const float g = (float)sigma12 / (float)sigma1_sq;\n"
            "            const VmafMtlGainTerms other_gain =",
        )
        self._assert_detected(failures, "fp32 arithmetic")

    def test_fp32_clamp_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "uint numer1 = (uint)(gain.sv_sq + (uint)sigma_nsq);",
            "uint numer1 = (uint)(gain.sv_sq + (uint)sigma_nsq);\n"
            "            g = min(g, vif_enhn_gain_limit);",
        )
        self._assert_detected(failures, "fp32 arithmetic")

    def test_gain_not_from_the_header_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "vmaf_mtl_ivif_gain_terms((uint)sigma1_sq",
            "other_terms((uint)sigma1_sq",
        )
        self._assert_detected(failures, "do not come from")

    def test_dropped_replay_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    if (!quick.replay) {\n        return quick.terms;\n    }\n",
            "    return quick.terms;\n",
        )
        self._assert_detected(failures, "must replay")

    def test_fp64_in_the_device_math_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    const vmaf_mtl_u64 product = (vmaf_mtl_u64)sigma12 * sigma12;",
            "    const vmaf_mtl_u64 product = (vmaf_mtl_u64)((double)sigma12 * (double)sigma12);",
        )
        self._assert_detected(failures, "fp64 type outside")

    def test_fp64_in_the_kernel_is_detected(self) -> None:
        failures = self._edited(KERNEL, "const int sigma_nsq = 65536 << 1;", "const double sigma_nsq = 131072.0;")
        self._assert_detected(failures, "fp64 type in a kernel")

    def test_reserved_identifier_is_detected(self) -> None:
        failures = self._edited(MATH, "const bool half_bit =", "const bool half =")
        self._assert_detected(failures, "Metal type or namespace")

    def test_long_long_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "vmaf_mtl_u64 product = (vmaf_mtl_u64)sigma12 * sigma12;",
            "vmaf_mtl_u64 product = (vmaf_mtl_u64)sigma12 * sigma12 + 0ULL;",
        )
        self._assert_detected(failures, "long long")

    def test_mul_hi_is_detected(self) -> None:
        failures = self._edited(
            SOFT,
            "VmafMtlU128 vmaf_mtl_u128_mul(vmaf_mtl_u64 a, vmaf_mtl_u64 b)\n{",
            "VmafMtlU128 vmaf_mtl_u128_mul(vmaf_mtl_u64 a, vmaf_mtl_u64 b)\n{\n"
            "    (void)mulhi(a, b);",
        )
        self._assert_detected(failures, "multiply-high")

    def test_changed_eps_is_detected(self) -> None:
        failures = self._edited(MATH, EPS_MANT, "0x1b7cdfd9d7bdba")
        self._assert_detected(failures, "eps constants")

    def test_changed_reference_is_detected(self) -> None:
        failures = self._edited(
            CPU,
            "            double g = sigma12 / (sigma1_sq + eps);",
            "            double g = sigma12 / (double)sigma1_sq;",
        )
        self._assert_detected(failures, CPU)

    def test_fp32_limit_binding_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "vmaf_mtl_ivif_make_gain_limit(s->vif_enhn_gain_limit)",
            "make_float_limit((float)s->vif_enhn_gain_limit)",
        )
        self._assert_detected(failures, "converted on the host")

    def test_changed_host_tail_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "const float fden = (float)(den_log / 2048.0 + (double)den_non_log);",
            "const float fden = (float)(den_log / 2048.0f + (double)den_non_log);",
        )
        self._assert_detected(failures, "host tail")


if __name__ == "__main__":
    unittest.main()
