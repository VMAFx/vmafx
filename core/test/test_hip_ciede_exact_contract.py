#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the ciede_hip design: the CPU's arithmetic in fp32 pairs, the CPU's sum (ADR-1448).

``ciede.c`` computes in double and stores in float, and adds every pixel's
value into one double in raster order. ``ciede_hip`` follows it with the
arithmetic of the SYCL twin (ADR-1436): ``feature/ciede_ff_math.h``, the
reference statement for statement with every fp64 value as an fp32 pair and
every fp64 math-library call as a pair function (``feature/ff_math.h``). That
header is pinned by ``test_sycl_ciede_exact_contract`` and held to the fp64
statements by ``test_hip_ciede_math`` and ``test_sycl_ciede_math``. This
contract pins what the HIP twin adds to it:

- the primitives the shared headers are built on (``ciede_hip_math.h`` and
  ``feature/ff_pair.h``): plain fp32 operators under the strict FP list, an
  explicit ``fmaf()``, fp32 root estimates;
- the kernels call the shared ``pixel()``, with the constants of the frame's
  bit depth, use no fp64 and no math function of their own, and store each
  value at its raster position with no reduction;
- the host reads the whole plane back, adds it with ``ciede_frame_sum()`` and
  forms the score with the reference's expression.

Device-free: reads the sources only. The planted regressions are constructs
the earlier twins had (fp32 formula with wave and block sums before ADR-1448;
fp64 device math in its first draft, 17 times the frame time).
``test_hip_ciede_parity`` checks the scores on a device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC_ROOT = ROOT / "core" / "src"

HOST = "feature/hip/ciede_hip.c"
KERNEL = "feature/hip/integer_ciede/ciede_score.hip"
PRIMITIVES = "feature/hip/integer_ciede/ciede_hip_math.h"
PAIR = "feature/ff_pair.h"
BUILD = "meson.build"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
FP64 = re.compile(r"\b(?:long\s+)?double\b")
# A math function in the kernel file: the arithmetic is the shared header's.
MATH_CALL = re.compile(
    r"\b(?:pow|cbrt|atan2|sin|cos|exp|log|sqrt|fabs)f?\s*\(|\b(?:fmaf|rintf|ldexpf)\s*\("
)
DEVICE_REDUCTION = re.compile(r"__shfl_\w+\s*\(|\batomicAdd\s*\(|__shared__|\bwarpSize\b")
FUNCTION_DEFINITION = re.compile(r"\b(?:Ff|float|bool|int)\s+\w+\([^;{]*\)\s*\{")
PRIMITIVE_LINES = (
    "#define VMAF_FF_INLINE inline __attribute__((always_inline))",
    "#define VMAF_FF_FMA(a, b, c) fmaf((a), (b), (c))",
    "#define VMAF_FF_FABS(x) fabsf(x)",
    "#define VMAF_FF_RINT(x) rintf(x)",
    "#define VMAF_FF_SQRT(x) sqrtf(x)",
    "#define VMAF_FF_CBRT(x) cbrtf(x)",
    "#define VMAF_FF_ROOT5(x) expf(0.2f * logf(x))",
    "#define VMAF_FF_LDEXP(x, k) ldexpf((x), (k))",
    '#include "feature/ff_pair.h"',
    "namespace vmaf_ffm_base = vmaf_ff_pair;",
    '#include "feature/ciede_ff_math.h"',
    "namespace vmaf_hip_ciede = vmaf_ciede_ff;",
)
PAIR_PIECES = (
    "return {.hi = product, .lo = VMAF_FF_FMA(a, b, -product)};",
    "VMAF_FF_INLINE float div_rn(float a, float b) { return a / b; }",
)
KERNEL_INCLUDE = '#include "feature/hip/integer_ciede/ciede_hip_math.h"'
# One set of ciede.c's constants per bit depth the engine reads, 8 to 16: entry
# `bpc - 8`, the host's ciede_hip_depth_index() (ADR-2145).
KERNEL_CONSTANTS = (
    "static constexpr vmaf_hip_ciede::Constants kCiedeConstants[9] = { "
    "vmaf_hip_ciede::make_constants(8u), vmaf_hip_ciede::make_constants(9u), "
    "vmaf_hip_ciede::make_constants(10u), vmaf_hip_ciede::make_constants(11u), "
    "vmaf_hip_ciede::make_constants(12u), vmaf_hip_ciede::make_constants(13u), "
    "vmaf_hip_ciede::make_constants(14u), vmaf_hip_ciede::make_constants(15u), "
    "vmaf_hip_ciede::make_constants(16u), };"
)
HOST_DEPTHS = (
    "#define CIEDE_HIP_DEPTH_NONE 9u",
    "return bpc >= 8u && bpc <= 16u ? bpc - 8u : CIEDE_HIP_DEPTH_NONE;",
    "unsigned depth = ciede_hip_depth_index(s->bpc);",
)
TERM_STORE = (
    "terms[(size_t)y * width + x] = vmaf_hip_ciede::pixel( ciede_samples<Sample>(ref, x, y, cx, cy), "
    "ciede_samples<Sample>(dis, x, y, cx, cy), kCiedeConstants[depth > 8u ? 8u : depth], tables);"
)
HOST_INCLUDE = '#include "ciede_frame_sum.h"'
HOST_CALL = "ciede_frame_sum((const float *)s->rb.host_pinned, (size_t)s->frame_w * s->frame_h);"
HOST_SCORE = "const double score = 45. - 20. * log10(de00_sum / (s->frame_w * s->frame_h));"
HOST_READBACK = "const size_t term_bytes = (size_t)s->frame_w * s->frame_h * sizeof(float);"
STRICT_FP = "hip_strict_fp_args = ['-ffp-contract=off', '-fhip-fp32-correctly-rounded-divide-sqrt']"
KERNEL_STANDARD = "'ciede_score' : ['-std=c++20'],"


def _flat(source: str) -> str:
    """Code without comments, every run of whitespace collapsed."""
    return " ".join(COMMENT.sub(" ", source).split())


def _sources() -> dict[str, str]:
    return {
        name: (SRC_ROOT / name).read_text(encoding="utf-8")
        for name in (HOST, KERNEL, PRIMITIVES, PAIR, BUILD)
    }


def _primitive_failures(sources: dict[str, str]) -> list[str]:
    primitives = sources[PRIMITIVES]
    failures = [
        f"{PRIMITIVES}: the HIP primitive is not `{line}`"
        for line in PRIMITIVE_LINES
        if line not in primitives
    ]
    code = _flat(primitives)
    if FP64.search(code):
        failures.append(f"{PRIMITIVES}: an fp64 type among the primitives")
    if FUNCTION_DEFINITION.search(code):
        failures.append(f"{PRIMITIVES}: a function definition next to the shared arithmetic")
    pair = _flat(sources[PAIR])
    for piece in PAIR_PIECES:
        if piece not in pair:
            failures.append(f"{PAIR}: the exact pair operation is not `{piece}`")
    return failures


def _kernel_failures(sources: dict[str, str]) -> list[str]:
    kernel = sources[KERNEL]
    code = _flat(kernel)
    failures: list[str] = []
    if KERNEL_INCLUDE not in kernel:
        failures.append(f"{KERNEL}: the kernels no longer compile the shared arithmetic")
    if FP64.search(code):
        failures.append(f"{KERNEL}: fp64 in the kernels (17 times the frame time, ADR-1448)")
    if MATH_CALL.search(code):
        failures.append(f"{KERNEL}: a math function of the kernel's own replaces the shared one")
    if KERNEL_CONSTANTS not in code:
        failures.append(f"{KERNEL}: the constants are not ciede.c's for 8, 10, 12 and 16 bits")
    if TERM_STORE not in code:
        failures.append(f"{KERNEL}: a pixel's value is not stored at its raster position")
    if DEVICE_REDUCTION.search(code):
        failures.append(f"{KERNEL}: the per-pixel values are reduced on the device")
    return failures


def _host_failures(sources: dict[str, str]) -> list[str]:
    host = sources[HOST]
    code = _flat(host)
    failures = [
        f"{HOST}: the bit-depth index of the kernels' constants is not `{line}`"
        for line in HOST_DEPTHS
        if line not in host
    ]
    if HOST_READBACK not in code:
        failures.append(f"{HOST}: the readback is no longer one float per pixel")
    if HOST_INCLUDE not in host or HOST_CALL not in code:
        failures.append(f"{HOST}: collect no longer adds the whole plane with ciede_frame_sum()")
    if HOST_SCORE not in code:
        failures.append(f"{HOST}: the score is not the reference's expression")
    if re.search(r"\+=\s*\(double\)", code):
        failures.append(f"{HOST}: the host adds device partials of its own")
    return failures


def _build_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    if STRICT_FP not in sources[BUILD]:
        failures.append(f"{BUILD}: the HIP kernels are no longer built without contraction")
    if KERNEL_STANDARD not in sources[BUILD]:
        failures.append(f"{BUILD}: ciede_score is not compiled as C++20")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _primitive_failures(sources)
        + _kernel_failures(sources)
        + _host_failures(sources)
        + _build_failures(sources)
    )


class CiedeHipExactContract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _contract_failures(sources)

    def _detects(self, failures: list[str], text: str) -> None:
        self.assertTrue(any(text in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_float_math_in_the_kernel_is_detected(self) -> None:
        # The pre-ADR-1448 xyz_to_lab_map().
        sources = _sources()
        sources[KERNEL] += "\nfloat lab_map(float t) { return cbrtf(t); }\n"
        self._detects(_contract_failures(sources), "math function of the kernel's own")

    def test_fp64_math_in_the_kernel_is_detected(self) -> None:
        # ADR-1448's first draft: the CUDA twin's fp64 statements.
        sources = _sources()
        sources[KERNEL] += "\nfloat lab_map(double t) { return (float)pow(t, 1.0 / 3.0); }\n"
        failures = _contract_failures(sources)
        self._detects(failures, "fp64 in the kernels")
        self._detects(failures, "math function of the kernel's own")

    def test_device_powf_as_root_estimate_is_detected(self) -> None:
        failures = self._edited(
            PRIMITIVES,
            "#define VMAF_FF_ROOT5(x) expf(0.2f * logf(x))",
            "#define VMAF_FF_ROOT5(x) powf((x), 0.2f)",
        )
        self._detects(failures, "HIP primitive is not")

    def test_two_prod_without_fma_is_detected(self) -> None:
        failures = self._edited(
            PAIR,
            "    return {.hi = product, .lo = VMAF_FF_FMA(a, b, -product)};",
            "    return {.hi = product, .lo = a * b - product};",
        )
        self._detects(failures, "exact pair operation")

    def test_reordered_constants_are_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "vmaf_hip_ciede::make_constants(10u), vmaf_hip_ciede::make_constants(11u),",
            "vmaf_hip_ciede::make_constants(11u), vmaf_hip_ciede::make_constants(10u),",
        )
        self._detects(failures, "constants are not ciede.c's")

    def test_bit_depth_passed_as_index_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    unsigned depth = ciede_hip_depth_index(s->bpc);",
            "    unsigned depth = s->bpc;",
        )
        self._detects(failures, "bit-depth index")

    def test_wave_reduction_is_detected(self) -> None:
        # The pre-ADR-1448 ciede_warp_reduce().
        sources = _sources()
        sources[KERNEL] += "\nfloat r(float v) { return v + __shfl_down(v, 16); }\n"
        self._detects(_contract_failures(sources), "reduced on the device")

    def test_block_sum_store_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "    terms[(size_t)y * width + x] =",
            "    terms[blockIdx.y * gridDim.x + blockIdx.x] +=",
        )
        self._detects(failures, "raster position")

    def test_host_sum_of_its_own_is_detected(self) -> None:
        # The pre-ADR-1448 collect_fex_hip().
        failures = self._edited(
            HOST,
            HOST_CALL,
            "0.0;\n    for (size_t i = 0; i < s->terms_capacity; i++)\n"
            "        total += (double)((const float *)s->rb.host_pinned)[i];",
        )
        self._detects(failures, "ciede_frame_sum()")
        self._detects(failures, "partials of its own")

    def test_other_score_expression_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            HOST_SCORE,
            "const double score = 45.0 - 20.0 * log10(de00_sum / ((double)s->frame_w * s->frame_h));",
        )
        self._detects(failures, "reference's expression")

    def test_contraction_in_the_kernel_build_is_detected(self) -> None:
        failures = self._edited(
            BUILD, STRICT_FP, "hip_strict_fp_args = ['-fhip-fp32-correctly-rounded-divide-sqrt']"
        )
        self._detects(failures, "without contraction")


if __name__ == "__main__":
    unittest.main()
