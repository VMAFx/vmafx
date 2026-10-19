#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the bit-exact integer_ssim_metal design (ADR-1498, after ADR-1443).

The CPU extractor (``integer_ssim.c``) forms each pixel's SSIM term in fp64
from int64 window moments and ``calc_ssim()`` adds every term into one
``double``, left to right and top to bottom, then divides by the sum of the
window weights. Metal has no fp64 type, so ``integer_ssim_metal``, like its
SYCL twin:

- runs the reference's fp64 operations, one for one and in the reference's
  order, on values held in 64-bit integers (``metal_integer_ssim_math.h`` on
  ``metal_soft_signed.h``), with no ``float`` and no ``double`` in the kernel
  or the header;
- stores the term's fp64 bit pattern of every pixel at its raster position,
  with no reduction on the device;
- adds the read-back plane on the host in index order and divides by the
  product of the two line weights; the dB options go through the CPU's
  helpers.

Device-free: reads the sources only. Every planted regression below is a
construct the pre-port twin had (fp32 term, ``simd_sum`` per work-group,
float partials, an inline dB ceiling) or a regrouping that rounds elsewhere,
so the contract fails on the old design and passes on the new one.
``test_metal_integer_ssim_math`` checks the arithmetic and the whole twin on
the host; ``test_metal_integer_ssim_parity`` checks the scores on a device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"

KERNEL = "metal/integer_ssim.metal"
HOST = "metal/integer_ssim_metal.mm"
MATH = "metal/metal_integer_ssim_math.h"
REFERENCE = "integer_ssim.c"
MATH_TEST = ROOT / "core" / "test" / "test_metal_integer_ssim_math.c"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
FLOAT_TYPE = re.compile(r"\bfloat\b|\bhalf\b")
DOUBLE_TYPE = re.compile(r"\bdouble\b")
REDUCTION = re.compile(r"\bsimd_sum\b|\bsimd_shuffle\w*|\bthreadgroup\b|\batomic\w*")

# The shared subset of MSL, C and C++ (metal_portable.h).
SUBSET = (
    (DOUBLE_TYPE, "uses double"),
    (re.compile(r"\blong\s+long\b"), "uses long long"),
    (re.compile(r"\b(?:0[xX][0-9A-Fa-f]+|\d+)(?:[uU]?[lL]+|[lL]+[uU])\b"), "has an L/LL literal"),
    (re.compile(r"\bstd::|\bnamespace\b|\btemplate\b|\bconstexpr\b"), "uses C++ only"),
    (re.compile(r"\bstatic\b"), "uses static"),
    (re.compile(r"[{,]\s*\.[A-Za-z_]\w*\s*="), "uses a designated initializer"),
    (re.compile(r"\bas_type\b|\bmemcpy\b|bit_cast"), "bit-casts outside VMAF_MTL_F2U/U2F"),
)

KERNEL_INCLUDE = '#include "metal_integer_ssim_math.h"'
KERNEL_NAME = "kernel void integer_ssim_vert_terms("
TERM_STORE = "terms[gid.y * width + gid.x] ="
TERM_CALL = (
    "vmaf_mtl_issim_term_bits(m, vmaf_mtl_issim_stabilisers(params.k1_bits, params.k2_bits));"
)
KERNEL_PARAMS = "constant VmafMtlIssimParams &params [[buffer(2)]]"
KERNEL_WEIGHT = "vmaf_mtl_issim_tap_weight(vmaf_mtl_issim_tap_range(gid.x, width))"

HOST_SUM = (
    "double sum = 0.0;",
    "for (size_t i = 0u; i < count; i++) {",
    "memcpy(&term, &terms[i], sizeof(term));",
    "sum += term;",
)
HOST_CALL = "issim_frame_sum(terms, (size_t)s->frame_w * s->frame_h);"
HOST_PIPELINE = '@"integer_ssim_vert_terms"'
HOST_PARAMS = "[enc setBytes:&s->params length:sizeof(s->params) atIndex:2];"
TERM_BUFFER = "pixels * sizeof(uint64_t)"
WEIGHT_SUM = "s->total_weight = issim_line_weight(w) * issim_line_weight(h);"
STABILISER = (
    "#define ISSIM_K1 (0.01 * 0.01)",
    "#define ISSIM_K2 (0.03 * 0.03)",
    "const double sm = (double)samplemax;",
    "const double value = sm * sm * k;",
)
HELPERS = (
    "s->max_db = vmaf_ssim_max_db(s->clip_db, bpc, w, h);",
    "vmaf_ssim_emit_ratio_score_named(",
    "(double)s->total_weight, s->enable_db, s->max_db,",
)

# The reference's operations, in its order, as the math header writes them.
TERM_STEPS = {
    "c1 is ((k1 * w) * w)": (
        "c1 =\n        vmaf_mtl_issim_times_weight(vmaf_mtl_issim_times_weight(k.k1, m.w), m.w);"
    ),
    "c2 is ((k2 * w) * w)": (
        "c2 =\n        vmaf_mtl_issim_times_weight(vmaf_mtl_issim_times_weight(k.k2, m.w), m.w);"
    ),
    "the numerator starts with w * (2 * mxy + c1)": (
        "vmaf_mtl_issim_times_weight(vmaf_mtl_signed_add(sums.twice_mxy, c1), m.w);"
    ),
    "the numerator is (w * a) * b": (
        "vmaf_mtl_signed_mul(means, vmaf_mtl_signed_add(c2, sums.twice_covariance));"
    ),
    "the denominator is (mx2 + my2 + c1) * (variances + c2)": (
        "vmaf_mtl_signed_add(sums.mean_squares, c1), vmaf_mtl_signed_add(sums.variances, c2));"
    ),
    "the term is one quotient": (
        "return vmaf_mtl_signed_bits(vmaf_mtl_signed_div(numerator, denominator));"
    ),
    "the variances are added left to right": (
        "vmaf_mtl_signed_sub(vmaf_mtl_signed_from_u64(p.x2w), mx2);"
    ),
    "the second variance joins before my2 leaves": (
        "vmaf_mtl_signed_add(reference_variance, vmaf_mtl_signed_from_u64(p.y2w));"
    ),
    "my2 is subtracted last": "vmaf_mtl_signed_add(mx2, my2), vmaf_mtl_signed_sub(both, my2)};",
    "the integer path stops at 2^52": (
        "#define VMAF_MTL_ISSIM_EXACT_PRODUCT_BOUND (VMAF_MTL_U64(1) << 52)"
    ),
    "the integer path is chosen by the products": "if (!vmaf_mtl_issim_products_are_exact(p)) {",
    "a full window's weight is 2^16": "#define VMAF_MTL_ISSIM_FULL_WEIGHT_LOG2 16",
}
GAUSSIAN = re.compile(r"vmaf_mtl_issim_kernel\[VMAF_MTL_ISSIM_TAPS\]\s*=\s*\{([^}]*)\}")
CPU_GAUSSIAN = [2, 9, 28, 55, 68, 55, 28, 9, 2]

# The lines of integer_ssim.c the header mirrors and the math test copies.
REFERENCE_LINES = (
    "#define SSIM_K1 (0.01 * 0.01)",
    "#define SSIM_K2 (0.03 * 0.03)",
    "w_d = (double)m.w;",
    "c1 = sm * sm * SSIM_K1 * w_d * w_d;",
    "c2 = sm * sm * SSIM_K2 * w_d * w_d;",
    "mx2 = m.mux * (double)m.mux;",
    "mxy = m.mux * (double)m.muy;",
    "my2 = m.muy * (double)m.muy;",
    "m.w * (2 * mxy + c1) * (c2 + 2 * (m.xy * w_d - mxy)) /",
    "((mx2 + my2 + c1) * (m.x2 * w_d - mx2 + m.y2 * w_d - my2 + c2));",
)
REFERENCE_SUM = (
    "*ssimw += m.w;",
    "return ssim / ssimw;",
    "gaussian_filter_init(&wk->vkernel, 1.5, 5)",
)


def _code(source: str) -> str:
    """The source with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", source)


def _sources() -> dict[str, str]:
    names = (KERNEL, HOST, MATH, REFERENCE)
    sources = {name: (FEATURE_ROOT / name).read_text(encoding="utf-8") for name in names}
    sources["test"] = MATH_TEST.read_text(encoding="utf-8")
    return sources


def _function_body(code: str, name: str) -> str:
    """Text of the first definition of `name` (brace-matched), or empty."""
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


def _kernel_failures(kernel: str) -> list[str]:
    failures: list[str] = []
    code = _code(kernel)
    for piece, what in (
        (KERNEL_INCLUDE, "the kernel does not include the math header"),
        (KERNEL_NAME, "the term kernel is gone"),
        (TERM_STORE, "the term is no longer stored at its raster position"),
        (TERM_CALL, "the term is not the header's fp64 term"),
        (KERNEL_PARAMS, "the stabilisers do not reach the kernel as fp64 bit patterns"),
        (KERNEL_WEIGHT, "the window weight is not the product of the tap sums"),
    ):
        if piece not in code:
            failures.append(f"{KERNEL}: {what}")
    if REDUCTION.search(code):
        failures.append(f"{KERNEL}: the terms are reduced on the device")
    if FLOAT_TYPE.search(code) or DOUBLE_TYPE.search(code):
        failures.append(f"{KERNEL}: the fixed-point twin computes in floating point")
    return failures


def _host_failures(host: str) -> list[str]:
    failures: list[str] = []
    code = _code(host)
    frame_sum = _function_body(code, "issim_frame_sum")
    for piece in HOST_SUM:
        if piece not in frame_sum:
            failures.append(f"{HOST}: the frame sum is not one double in raster order ({piece})")
    for piece, what in (
        (HOST_CALL, "collect no longer adds the whole term plane"),
        (HOST_PIPELINE, "the host does not build the term kernel"),
        (HOST_PARAMS, "the kernel arguments are not the shared VmafMtlIssimParams"),
        (TERM_BUFFER, "the readback is not one fp64 pattern per pixel"),
        (WEIGHT_SUM, "the weight sum is not the product of the two line sums"),
    ):
        if piece not in code:
            failures.append(f"{HOST}: {what}")
    for piece in STABILISER:
        if piece not in code:
            failures.append(
                f"{HOST}: the stabilisers are not the reference's sm * sm * K ({piece})"
            )
    for piece in HELPERS:
        if piece not in code:
            failures.append(f"{HOST}: the dB options do not go through the CPU's helpers ({piece})")
    if FLOAT_TYPE.search(code):
        failures.append(f"{HOST}: the host reads or adds float partials")
    return failures


def _math_failures(math: str) -> list[str]:
    failures: list[str] = []
    code = _code(math)
    for what, line in TERM_STEPS.items():
        if line not in code:
            failures.append(f"{MATH}: {what}: `{line.strip()}` is gone")
    if FLOAT_TYPE.search(code):
        failures.append(f"{MATH}: the term uses a floating-point type")
    for pattern, what in SUBSET:
        if pattern.search(code):
            failures.append(f"{MATH}: leaves the MSL/C/C++ subset: {what}")
    if '#include "metal_soft_signed.h"' not in code:
        failures.append(f"{MATH}: the term is not built on metal_soft_signed.h")
    match = GAUSSIAN.search(code)
    taps = [int(tap) for tap in re.findall(r"\d+", match.group(1))] if match else []
    if taps != CPU_GAUSSIAN:
        failures.append(f"{MATH}: the Gaussian is not gaussian_filter_init(1.5, 5)'s")
    return failures


def _reference_failures(reference: str, test: str) -> list[str]:
    failures: list[str] = []
    for line in REFERENCE_LINES:
        if line not in reference:
            failures.append(f"{REFERENCE}: no longer holds `{line}`; the twin mirrors it")
        if line not in test:
            failures.append(f"{MATH_TEST.name}: reference_term() lost `{line}`")
    for line in REFERENCE_SUM:
        if line not in reference:
            failures.append(f"{REFERENCE}: no longer holds `{line}`; the twin mirrors it")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _kernel_failures(sources[KERNEL])
        + _host_failures(sources[HOST])
        + _math_failures(sources[MATH])
        + _reference_failures(sources[REFERENCE], sources["test"])
    )


def _replaced(name: str, old: str, new: str) -> list[str]:
    """The contract's failures with `old` replaced by `new` in one source."""
    sources = _sources()
    if old not in sources[name]:
        raise AssertionError(f"{name}: `{old}` not found, the planted regression is stale")
    sources[name] = sources[name].replace(old, new, 1)
    return _contract_failures(sources)


def _appended(name: str, text: str) -> list[str]:
    sources = _sources()
    sources[name] += text
    return _contract_failures(sources)


class IntegerSsimMetalExactContract(unittest.TestCase):
    def _assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_simd_sum_reduction_is_detected(self) -> None:
        # The pre-port per-work-group simd_sum of the terms.
        failures = _appended(KERNEL, "\ninline float r(float t) { return simd_sum(t); }\n")
        self._assert_detected(failures, "reduced on the device")

    def test_float_term_is_detected(self) -> None:
        # The pre-port fp32 term: `const float w_d = (float)w;`.
        failures = _appended(KERNEL, "\ninline float w_d(long w) { return (float)w; }\n")
        self._assert_detected(failures, "computes in floating point")

    def test_unstored_term_is_detected(self) -> None:
        failures = _replaced(KERNEL, TERM_STORE, "const ulong term =")
        self._assert_detected(failures, "raster position")

    def test_float_partials_on_the_host_are_detected(self) -> None:
        # The pre-port collect(): `const float *parts = ...rb.host_view;`.
        failures = _appended(HOST, "\nstatic double p(const float *parts) { return parts[0]; }\n")
        self._assert_detected(failures, "float partials")

    def test_host_sum_of_a_part_is_detected(self) -> None:
        failures = _replaced(HOST, HOST_CALL, "issim_frame_sum(terms, s->frame_w);")
        self._assert_detected(failures, "whole term plane")

    def test_reordered_host_sum_is_detected(self) -> None:
        failures = _replaced(
            HOST, "for (size_t i = 0u; i < count; i++) {", "for (size_t i = count; i-- > 0u;) {"
        )
        self._assert_detected(failures, "raster order")

    def test_summed_weight_plane_is_detected(self) -> None:
        failures = _replaced(HOST, WEIGHT_SUM, "s->total_weight = 0;")
        self._assert_detected(failures, "product of the two line sums")

    def test_inline_db_ceiling_is_detected(self) -> None:
        # The pre-port init() computed the ceiling itself.
        failures = _replaced(HOST, HELPERS[0], "s->max_db = ceil(10.0 * log10((double)(w * h)));")
        self._assert_detected(failures, "CPU's helpers")

    def test_fp32_stabiliser_is_detected(self) -> None:
        failures = _replaced(HOST, STABILISER[3], "const double value = (float)(sm * sm) * k;")
        self._assert_detected(failures, "sm * sm * K")

    def test_regrouped_numerator_is_detected(self) -> None:
        # w * (a * b) rounds elsewhere than (w * a) * b on an edge window.
        failures = _replaced(
            MATH,
            "vmaf_mtl_issim_times_weight(vmaf_mtl_signed_add(sums.twice_mxy, c1), m.w);",
            "vmaf_mtl_signed_add(sums.twice_mxy, c1);",
        )
        self._assert_detected(failures, "starts with w * (2 * mxy + c1)")

    def test_squared_weight_is_detected(self) -> None:
        # k * (w * w) is one rounding where the reference has two.
        failures = _replaced(
            MATH,
            "vmaf_mtl_issim_times_weight(vmaf_mtl_issim_times_weight(k.k1, m.w), m.w);",
            "vmaf_mtl_signed_mul(k.k1, vmaf_mtl_signed_from_u64(m.w * m.w));",
        )
        self._assert_detected(failures, "c1 is ((k1 * w) * w)")

    def test_regrouped_variances_are_detected(self) -> None:
        failures = _replaced(
            MATH,
            "vmaf_mtl_signed_add(reference_variance, vmaf_mtl_signed_from_u64(p.y2w));",
            "vmaf_mtl_signed_add(reference_variance,"
            " vmaf_mtl_signed_sub(vmaf_mtl_signed_from_u64(p.y2w), my2));",
        )
        self._assert_detected(failures, "joins before my2 leaves")

    def test_wider_integer_path_is_detected(self) -> None:
        # At 2^53 a sum of two products is no longer an fp64 value.
        failures = _replaced(MATH, "(VMAF_MTL_U64(1) << 52)", "(VMAF_MTL_U64(1) << 53)")
        self._assert_detected(failures, "stops at 2^52")

    def test_double_in_the_header_is_detected(self) -> None:
        failures = _appended(MATH, "\nVMAF_MTL_FUNC double half(void) { return 0.5; }\n")
        self._assert_detected(failures, "uses double")

    def test_changed_gaussian_is_detected(self) -> None:
        failures = _replaced(MATH, "= {2,  9,  28, 55, 68,", "= {2,  9,  27, 55, 68,")
        self._assert_detected(failures, "gaussian_filter_init(1.5, 5)")

    def test_changed_reference_is_detected(self) -> None:
        failures = _replaced(
            REFERENCE,
            "c1 = sm * sm * SSIM_K1 * w_d * w_d;",
            "c1 = sm * sm * SSIM_K1 * (w_d * w_d);",
        )
        self._assert_detected(failures, "the twin mirrors it")


if __name__ == "__main__":
    unittest.main()
