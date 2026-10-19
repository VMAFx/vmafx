#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the CPU arithmetic of ciede_sycl (ADR-1436).

``ciede.c`` computes in double and stores in float. ``ciede_sycl`` follows it
statement for statement on a device that has no fp64 type (ADR-0220):

* every fp64 value is an fp32 pair and every fp64 math-library call a pair
  function (``feature/ff_math.h``), rounded to float where the reference
  rounds (``feature/ciede_ff_math.h``). Both headers are shared with the HIP
  twin (ADR-1448); ``sycl/sycl_ff_math.h`` and ``sycl/sycl_ciede_math.h`` name
  the SYCL primitives they are built on and include them;
* the kernel has no call left in it (a call frame is scratch memory,
  ADR-1395) and reads the two tables from device memory;
* it stores one float per pixel, and the host adds them into one double in
  the reference's raster order (``feature/ciede_frame_sum.h``) and applies the
  reference's score expression.

Device-free: reads the sources only. ``test_sycl_ciede_math`` checks the
arithmetic on the host and on a device, ``test_sycl_ciede_parity`` the scores
on a device.
"""

from __future__ import annotations

import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"
GENERATOR = ROOT / "scripts" / "dev" / "gen_sycl_ff_math.py"

TWIN = "sycl/integer_ciede_sycl.cpp"
# The arithmetic, shared with the HIP twin.
MATH = "ciede_ff_math.h"
FF = "ff_math.h"
SUM = "ciede_frame_sum.h"
# The SYCL primitives the shared headers are built on.
SYCL_MATH = "sycl/sycl_ciede_math.h"
SYCL_FF = "sycl/sycl_ff_math.h"
CPU = "ciede.c"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
SPACE = re.compile(r"\s+")
FP64 = re.compile(r"\b(?:long\s+)?double\b")
# Significand bits of x87 extended precision, which the generator needs.
EXTENDED_MANTISSA_BITS = 63
# Host code in the ciede header that may name the fp64 type.
HOST_ONLY = ("make_pair", "make_constants")
# The reference's lines the header mirrors.
REFERENCE_LINES = (
    "return pow(x, 2.4);",
    "return pow(c, 1.0 / 3.0);",
    "if (c > 10. / 255.) {",
    "float hue_angle = (float)atan2(x, y);",
    "const float c1 = (float)sqrt(square(color_1.a) + square(color_1.b));",
    "sin(degrees_to_radians((float)(60.0 * exp(-(degrees * degrees))))));",
    # Upstream's two float products (Netflix/vmaf libvmaf/src/feature/ciede.c:224-225
    # and :235-236; the squares are products since ADR-1467). ADR-1476. The
    # explicit (double) converts the product's result, as upstream's implicit
    # promotion does; an operand is never cast.
    "const float delta_upcase_h_prime = (float)(2.0 * sqrt((double)(c_prime_1 * c_prime_2)) * sin(delta_h_prime / 2.0));",
    "return (float)(sqrt(square(lightness) + square(chroma) + square(hue) + (double)(r_sub_t * chroma * hue)));",
    "de00_sum += ciede2000(",
    "const double score = 45. - 20. * log10(de00_sum / (ref_pic->w[0] * ref_pic->h[0]));",
)
# The reference's fp64 constants make_constants() repeats.
SHARED_LITERALS = (
    "1.28033",
    "0.21482",
    "0.38059",
    "2.12798",
    "0.4124564390896921",
    "0.357576077643909",
    "0.18043748326639894",
    "0.21267285140562248",
    "0.715152155287818",
    "0.07217499330655958",
    "0.019333895582329317",
    "0.119192025881303",
    "0.9503040785363677",
    "0.95047",
    "1.08883",
    "24389.0 / 27.0",
    "216.0 / 24389.0",
    "1.0 / 116.0",
    "1.0 / 1.055",
    "1.0 / 12.92",
)
# The header's statements where the reference's types and order show.
MATH_PIECES = (
    ("rgb_to_xyz_map", "if (less(k.gamma_knee, c)) {"),
    (
        "rgb_to_xyz_map",
        "vmaf_ffm::pow_2_4(ff_mul(ff_add(c, k.gamma_offset), k.gamma_gain))",
    ),
    ("xyz_to_lab_map", "return to_float(vmaf_ffm::cbrt(c));"),
    ("h_prime", "vmaf_ffm::atan2(x, y, tables.atan)"),
    ("r_sub_t", "const float exponent = -(degrees * degrees);"),
    ("r_sub_t", "const float ratio = div_rn(c7, c7 + kPowf25To7);"),
    # The reference's float products (ADR-1476): rounded to float, then widened.
    ("delta_e", "const Ff chord = vmaf_ffm::sqrt(from_float(c_prime_1 * c_prime_2));"),
    ("delta_e", "const float cross = rotation * chroma * hue;"),
    ("delta_e", "return to_float(vmaf_ffm::sqrt(add_f(squares, cross)));"),
)
FP32_LIBM = re.compile(r"sycl::(?:pow|cbrt|atan2|sin|cos|exp|log|sqrt)\(")
# An fp32 math function or root-estimate primitive in the shared arithmetic,
# where the reference computes in fp64. ff_math.h is where the estimates are
# corrected; ciede_ff_math.h must not use one directly.
SHARED_FP32_LIBM = re.compile(
    r"\bVMAF_FF_(?:SQRT|CBRT|ROOT5)\(|\b(?:powf|cbrtf|atan2f|sinf|cosf|expf|logf|sqrtf)\("
)
# What the SYCL wrappers hand the shared headers.
SYCL_PRIMITIVES = (
    (SYCL_FF, "namespace vmaf_ffm_base = vmaf_sycl_exact;"),
    (SYCL_FF, "#define VMAF_FF_INLINE VMAF_SYCL_ALWAYS_INLINE"),
    (SYCL_FF, "#define VMAF_FF_FABS(x) sycl::fabs(x)"),
    (SYCL_FF, "#define VMAF_FF_RINT(x) sycl::rint(x)"),
    (SYCL_FF, "#define VMAF_FF_SQRT(x) sycl::sqrt(x)"),
    (SYCL_FF, "#define VMAF_FF_CBRT(x) sycl::cbrt(x)"),
    (SYCL_FF, "#define VMAF_FF_ROOT5(x) sycl::pow(x, 0.2f)"),
    (SYCL_FF, '#include "../ff_math.h"'),
    (SYCL_FF, "namespace vmaf_sycl_ffm = vmaf_ffm;"),
    (SYCL_MATH, "#define VMAF_FF_LDEXP(x, k) sycl::ldexp(x, k)"),
    (SYCL_MATH, '#include "../ciede_ff_math.h"'),
    (SYCL_MATH, "namespace vmaf_sycl_ciede = vmaf_ciede_ff;"),
)


def _code(source: str) -> str:
    """The source without comments and with whitespace collapsed."""
    return SPACE.sub(" ", COMMENT.sub(" ", source))


def _sources() -> dict[str, str]:
    return {
        name: (FEATURE_ROOT / name).read_text(encoding="utf-8")
        for name in (TWIN, MATH, FF, SUM, SYCL_MATH, SYCL_FF, CPU)
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


def _reference_failures(sources: dict[str, str]) -> list[str]:
    cpu = _code(sources[CPU])
    failures = [f"{CPU}: no longer holds `{line}`" for line in REFERENCE_LINES if line not in cpu]
    constants = _function_body(_code(sources[MATH]), "make_constants")
    for literal in SHARED_LITERALS:
        if literal not in cpu:
            failures.append(f"{CPU}: no longer holds the constant `{literal}`")
        if literal not in constants:
            failures.append(f"{MATH}: make_constants() does not build `{literal}`")
    return failures


def _math_failures(sources: dict[str, str]) -> list[str]:
    math = _code(sources[MATH])
    failures: list[str] = []
    for function, piece in MATH_PIECES:
        if piece not in _function_body(math, function):
            failures.append(f"{MATH}: {function}() is not the reference's statement ({piece})")
    device = math
    for name in HOST_ONLY:
        device = device.replace(_function_body(math, name), " ")
    if FP64.search(device):
        failures.append(f"{MATH}: fp64 type outside the host-only helpers (ADR-0220)")
    if FP64.search(_code(sources[FF])):
        failures.append(f"{FF}: fp64 type in kernel code (ADR-0220)")
    if FP32_LIBM.search(device) or SHARED_FP32_LIBM.search(device):
        failures.append(f"{MATH}: an fp32 math-library call where the reference computes in fp64")
    for name in (MATH, FF):
        if "sycl::" in _code(sources[name]) or "<sycl/" in sources[name]:
            failures.append(f"{name}: the shared arithmetic names a SYCL function (ADR-1448)")
    return failures


def _wrapper_failures(sources: dict[str, str]) -> list[str]:
    """The SYCL wrappers define the primitives and nothing of the arithmetic."""
    failures = [
        f"{name}: the SYCL primitive is not `{piece}`"
        for name, piece in SYCL_PRIMITIVES
        if piece not in sources[name]
    ]
    for name in (SYCL_MATH, SYCL_FF):
        code = _code(sources[name])
        if FP64.search(code):
            failures.append(f"{name}: fp64 type in kernel code (ADR-0220)")
        if re.search(r"\b(?:Ff|float|bool)\s+\w+\(", code):
            failures.append(f"{name}: a function definition next to the shared arithmetic")
    return failures


def _table_failures(sources: dict[str, str]) -> list[str]:
    """Kernels read the tables through a pointer into device memory."""
    ff = _code(sources[FF])
    failures: list[str] = []
    for function in ("sin_cos", "atan2"):
        body = _function_body(ff, function)
        if "table[index" not in body or "kSinCosTable[" in body or "kAtanTable[" in body:
            failures.append(
                f"{FF}: {function}() must read its table from the pointer it is given "
                "(ADR-1395: a constant array indexed at run time is scratch memory)"
            )
    twin = _code(sources[TWIN])
    for table in ("kAtanTable", "kSinCosTable"):
        if f"vmaf_sycl_ffm::{table}, sizeof(vmaf_sycl_ffm::{table})" not in twin:
            failures.append(f"{TWIN}: {table} is not copied to device memory")
    return failures


def _twin_failures(sources: dict[str, str]) -> list[str]:
    twin = _code(sources[TWIN])
    failures: list[str] = []
    if "__attribute__((flatten, always_inline)) static inline float ciede_pixel(" not in twin:
        failures.append(
            f"{TWIN}: ciede_pixel() must be flattened into the kernel (a call frame is "
            "scratch memory, ADR-1395)"
        )
    if "a_.terms[id[0] * (size_t)a_.width + id[1]] = ciede_pixel(a_, id[1], id[0]);" not in twin:
        failures.append(f"{TWIN}: the kernel must store every pixel at its raster position")
    if (
        "static constexpr int CIEDE_SYCL_SG = 16;" not in twin
        or "static constexpr int CIEDE_SYCL_GRF = 0;" not in twin
        or "class CiedeKernel : public VmafSyclKernelShape<CIEDE_SYCL_SG, CIEDE_SYCL_GRF>"
        not in twin
    ):
        failures.append(
            f"{TWIN}: the kernel shape must stay SIMD-16 with the default register file "
            "(SIMD-32 spills to scratch memory, ADR-1395)"
        )
    if re.search(r"reduce_over_group|sycl::reduction|sycl::plus", twin):
        failures.append(f"{TWIN}: a device reduction adds in another order than extract()")
    total = _function_body(_code(sources[SUM]), "ciede_frame_sum")
    if "double de00_sum = 0.0;" not in total or "de00_sum += (double)terms[i];" not in total:
        failures.append(f"{SUM}: the host must add the pixels into one double, in raster order")
    if '#include "feature/ciede_frame_sum.h"' not in sources[TWIN] or re.search(
        r"double ciede_frame_sum\(", twin
    ):
        failures.append(f"{TWIN}: the frame sum is not the shared ciede_frame_sum()")
    if "ciede_frame_sum(s->h_terms, (size_t)s->width * s->height)" not in twin:
        failures.append(f"{TWIN}: collect no longer adds the whole plane")
    if "45. - 20. * std::log10(de00_sum / (s->width * s->height))" not in twin:
        failures.append(f"{TWIN}: the score expression is not extract()'s")
    if "vmaf_sycl_ciede::pixel(" not in _function_body(twin, "ciede_pixel"):
        failures.append(f"{TWIN}: the pixel does not come from sycl_ciede_math.h")
    if FP32_LIBM.search(twin.replace("std::log10(", " ")):
        failures.append(f"{TWIN}: an fp32 math-library call in the twin")
    return failures


def _failures(sources: dict[str, str]) -> list[str]:
    return (
        _reference_failures(sources)
        + _math_failures(sources)
        + _wrapper_failures(sources)
        + _table_failures(sources)
        + _twin_failures(sources)
    )


class SyclCiedeExactContractTest(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _failures(sources)

    def _detects(self, failures: list[str], text: str) -> None:
        self.assertTrue(any(text in failure for failure in failures), failures)

    def test_live_sources_keep_the_cpu_arithmetic(self) -> None:
        self.assertEqual(_failures(_sources()), [])

    def test_generated_constants_are_current(self) -> None:
        """The header's constants and tables are what the generator writes."""
        try:
            import numpy as np  # noqa: PLC0415
        except ImportError:
            self.skipTest("numpy is not installed")
        if np.finfo(np.longdouble).nmant < EXTENDED_MANTISSA_BITS:
            self.skipTest("numpy.longdouble has no extended precision on this host")
        # The interpreter and the script path are this test's own; no shell.
        result = subprocess.run(  # noqa: S603
            [sys.executable, str(GENERATOR), "--check"],
            capture_output=True,
            text=True,
            check=False,
            timeout=20,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_fp32_gamma_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "        return vmaf_ffm::pow_2_4(ff_mul(ff_add(c, k.gamma_offset), k.gamma_gain));",
            "        return from_float(powf(to_float(c), 2.4f));",
        )
        self._detects(failures, "rgb_to_xyz_map()")
        self._detects(failures, "fp32 math-library call")

    def test_swapped_atan2_arguments_are_detected(self) -> None:
        failures = self._edited(
            MATH,
            "vmaf_ffm::atan2(x, y, tables.atan)",
            "vmaf_ffm::atan2(y, x, tables.atan)",
        )
        self._detects(failures, "h_prime()")

    def test_fp64_in_the_device_math_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    const float exponent = -(degrees * degrees);",
            "    const float exponent = (float)-((double)degrees * (double)degrees);",
        )
        self._detects(failures, "fp64 type outside")

    def test_exact_chroma_product_is_detected(self) -> None:
        # The form between PR #552 and ADR-1476: the product kept in fp64.
        failures = self._edited(
            MATH,
            "    const Ff chord = vmaf_ffm::sqrt(from_float(c_prime_1 * c_prime_2));",
            "    const Ff chord = vmaf_ffm::sqrt(two_prod(c_prime_1, c_prime_2));",
        )
        self._detects(failures, "delta_e()")

    def test_exact_rotation_product_is_detected(self) -> None:
        # The form between PR #552 and ADR-1476: the product kept in fp64.
        failures = self._edited(
            MATH,
            "    const float cross = rotation * chroma * hue;",
            "    const Ff cross = mul_f(two_prod(rotation, chroma), hue);",
        )
        self._detects(failures, "delta_e()")

    def test_widened_reference_product_is_detected(self) -> None:
        # PR #552's cast: not upstream's statement.
        failures = self._edited(
            CPU,
            "(double)(r_sub_t * chroma * hue)",
            "(double)r_sub_t * chroma * hue",
        )
        self._detects(failures, CPU)
        failures = self._edited(
            CPU, "sqrt((double)(c_prime_1 * c_prime_2))", "sqrt((double)c_prime_1 * c_prime_2)"
        )
        self._detects(failures, CPU)

    def test_changed_constant_is_detected(self) -> None:
        failures = self._edited(MATH, "make_pair(1.28033)", "make_pair(1.28034)")
        self._detects(failures, "make_constants() does not build `1.28033`")

    def test_constant_table_in_a_kernel_is_detected(self) -> None:
        failures = self._edited(
            FF,
            "    const Ff sin_k = {.hi = table[index + 0u], .lo = table[index + 1u]};",
            "    const Ff sin_k = {.hi = kSinCosTable[index + 0u], .lo = kSinCosTable[index + 1u]};",
        )
        self._detects(failures, "sin_cos() must read its table")

    def test_call_frames_in_the_kernel_are_detected(self) -> None:
        failures = self._edited(
            TWIN,
            "__attribute__((flatten, always_inline)) static inline float",
            "static inline float",
        )
        self._detects(failures, "flattened into the kernel")

    def test_block_reduction_is_detected(self) -> None:
        failures = self._edited(
            TWIN,
            "        a_.terms[id[0] * (size_t)a_.width + id[1]] = ciede_pixel(a_, id[1], id[0]);",
            "        a_.terms[0] = sycl::reduce_over_group(g, ciede_pixel(a_, id[1], id[0]),\n"
            "                                              sycl::plus<float>{});",
        )
        self._detects(failures, "raster position")
        self._detects(failures, "device reduction")

    def test_simd32_kernel_shape_is_detected(self) -> None:
        failures = self._edited(
            TWIN,
            "static constexpr int CIEDE_SYCL_SG = 16;",
            "static constexpr int CIEDE_SYCL_SG = 32;",
        )
        self._detects(failures, "kernel shape")

    def test_float_frame_sum_is_detected(self) -> None:
        failures = self._edited(SUM, "    double de00_sum = 0.0;", "    float de00_sum = 0.0f;")
        self._detects(failures, "one double, in raster order")

    def test_frame_sum_of_its_own_is_detected(self) -> None:
        failures = self._edited(
            TWIN,
            '#include "feature/ciede_frame_sum.h"\n',
            "static double ciede_frame_sum(const float *terms, size_t count);\n",
        )
        self._detects(failures, "shared ciede_frame_sum()")

    def test_sycl_function_in_the_shared_arithmetic_is_detected(self) -> None:
        failures = self._edited(
            FF,
            "    const float k = VMAF_FF_RINT(x * kInvLn2);",
            "    const float k = sycl::rint(x * kInvLn2);",
        )
        self._detects(failures, "names a SYCL function")

    def test_changed_sycl_primitive_is_detected(self) -> None:
        failures = self._edited(
            SYCL_FF,
            "#define VMAF_FF_ROOT5(x) sycl::pow(x, 0.2f)",
            "#define VMAF_FF_ROOT5(x) sycl::exp(0.2f * sycl::log(x))",
        )
        self._detects(failures, "SYCL primitive is not")

    def test_arithmetic_in_a_sycl_wrapper_is_detected(self) -> None:
        sources = _sources()
        sources[SYCL_MATH] += "\ninline float lab_map(float t) { return sycl::cbrt(t); }\n"
        self._detects(_failures(sources), "function definition next to the shared arithmetic")

    def test_changed_reference_is_detected(self) -> None:
        failures = self._edited(CPU, "    return pow(x, 2.4);", "    return powf(x, 2.4f);")
        self._detects(failures, CPU)


if __name__ == "__main__":
    unittest.main()
