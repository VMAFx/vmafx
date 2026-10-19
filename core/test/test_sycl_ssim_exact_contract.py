#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the bit-exact integer_ssim_sycl design (ADR-1443).

The CPU extractor (``integer_ssim.c``) forms each pixel's SSIM term in fp64
from int64 window moments and ``calc_ssim()`` adds every term into one
``double``, left to right and top to bottom. A SYCL kernel has no fp64 type
(ADR-0220), so ``integer_ssim_sycl``:

- runs the reference's fp64 operations, one for one and in the reference's
  order, on values held in 64-bit integers (``sycl_integer_ssim_math.h`` on
  ``sycl_soft_signed.h``), with no ``float`` and no ``double`` in the term;
- stores the term's fp64 bit pattern of every pixel at its raster position,
  with no reduction on the device;
- adds the read-back plane on the host in index order.

Device-free: reads the sources only. Every planted regression below is a
construct the pre-ADR-1443 twin had or a regrouping that rounds elsewhere, so
the contract fails on the old design and passes on the new one.
``test_sycl_integer_ssim_math`` checks the arithmetic against the reference's
expression and ``test_sycl_ssim_parity`` checks the scores on a device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"

TWIN = "sycl/integer_ssim_sycl.cpp"
MATH = "sycl/sycl_integer_ssim_math.h"
SOFT = "sycl/sycl_soft_signed.h"
REFERENCE = "integer_ssim.c"
MATH_TEST = ROOT / "core" / "test" / "test_sycl_integer_ssim_math.c"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# The fixed-point twin shares its file with float_ssim_sycl; it starts here.
SECTION = "Real integer_ssim SYCL extractor (ADR-0564"
FLOAT_TYPE = re.compile(r"\bfloat\b")
DOUBLE_TYPE = re.compile(r"\bdouble\b")
PLAIN_INLINE = re.compile(r"^inline\s+(?!constexpr)", re.M)

TERM_STORE = "a_.terms[id[0] * (size_t)a_.width + id[1]] ="
KERNEL_SHAPE = "class IssimTermKernel : public VmafSyclKernelShape<ISSIM_TERM_SG, ISSIM_TERM_GRF>"
SHAPE_VALUES = ("constexpr int ISSIM_TERM_SG = 0;", "constexpr int ISSIM_TERM_GRF = 256;")
FLATTENED = "__attribute__((flatten, always_inline)) static inline uint64_t\ninteger_ssim_term("
# frame_sum_of_terms() is shared with float_ssim_sycl (ADR-1463) and sits
# above the fixed-point twin's section.
HOST_SUM = (
    "double sum = 0.0;",
    "for (size_t i = 0U; i < count; i++)",
    "sum += std::bit_cast<double>(terms[i]);",
)
HOST_CALL = "frame_sum_of_terms(s->h_terms, (size_t)s->width * s->height);"
READBACK = "(size_t)s->width * s->height * sizeof(uint64_t)"
WEIGHT_SUM = "s->total_weight = line_weight(width) * line_weight(height);"

# The reference's operations, in its order, as the math header writes them.
TERM_STEPS = {
    "c1 is ((k1 * w) * w)": "c1 = times_weight(times_weight(k.k1, m.w), m.w);",
    "c2 is ((k2 * w) * w)": "c2 = times_weight(times_weight(k.k2, m.w), m.w);",
    "the numerator starts with w * (2 * mxy + c1)": (
        "means = times_weight(signed_add(sums.twice_mxy, c1), m.w);"
    ),
    "the numerator is (w * a) * b": (
        "numerator = signed_mul(means, signed_add(c2, sums.twice_covariance));"
    ),
    "the denominator is (mx2 + my2 + c1) * (variances + c2)": (
        "signed_mul(signed_add(sums.mean_squares, c1), signed_add(sums.variances, c2));"
    ),
    "the term is one quotient": "return signed_bits(signed_div(numerator, denominator));",
    "the variances are added left to right": (
        "reference_variance = signed_sub(signed_from_u64(p.x2w), mx2);"
    ),
    "the second variance joins before my2 leaves": (
        "both = signed_add(reference_variance, signed_from_u64(p.y2w));"
    ),
    "my2 is subtracted last": ".variances = signed_sub(both, my2)};",
    "the integer path stops at 2^52": "kExactProductBound = uint64_t{1} << 52;",
    "the integer path is chosen by the products": "if (!products_are_exact(p)) {",
}

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
REFERENCE_SUM = ("*ssimw += m.w;", "return ssim / ssimw;")


def _code(source: str) -> str:
    """The source with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", source)


def _sources() -> dict[str, str]:
    names = (TWIN, MATH, SOFT, REFERENCE)
    sources = {name: (FEATURE_ROOT / name).read_text(encoding="utf-8") for name in names}
    sources["test"] = MATH_TEST.read_text(encoding="utf-8")
    return sources


def _integer_section(twin: str) -> str:
    return _code(twin[twin.index(SECTION) :])


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


def _kernel_failures(twin: str) -> list[str]:
    failures: list[str] = []
    code = _integer_section(twin)
    if TERM_STORE not in code:
        failures.append(f"{TWIN}: the term is no longer stored at its raster position")
    if "reduce_over_group" in code:
        failures.append(f"{TWIN}: the terms are reduced on the device")
    if FLOAT_TYPE.search(code):
        failures.append(f"{TWIN}: the fixed-point twin computes in float")
    if KERNEL_SHAPE not in code or any(value not in code for value in SHAPE_VALUES):
        failures.append(
            f"{TWIN}: the term kernel does not leave its sub-group size to the compiler with the "
            "256-entry register file"
        )
    if FLATTENED not in code:
        failures.append(f"{TWIN}: the term is not flattened into the kernel")
    return failures


def _host_failures(twin: str) -> list[str]:
    failures: list[str] = []
    code = _integer_section(twin)
    shared_sum = _function_body(_code(twin), "frame_sum_of_terms")
    for piece in HOST_SUM:
        if piece not in shared_sum:
            failures.append(f"{TWIN}: the frame sum is not one double in raster order ({piece})")
    if HOST_CALL not in code:
        failures.append(f"{TWIN}: collect no longer adds the whole term plane")
    if READBACK not in code:
        failures.append(f"{TWIN}: the readback is not one fp64 pattern per pixel")
    if WEIGHT_SUM not in code:
        failures.append(f"{TWIN}: the weight sum is not the product of the two line sums")
    return failures


def _math_failures(math: str, soft: str) -> list[str]:
    failures: list[str] = []
    code = _code(math)
    for what, line in TERM_STEPS.items():
        if line not in code:
            failures.append(f"{MATH}: {what}: `{line}` is gone")
    kernel_code = code[code.index("struct Products") :]
    if DOUBLE_TYPE.search(kernel_code) or FLOAT_TYPE.search(kernel_code):
        failures.append(f"{MATH}: the term uses a floating-point type")
    if PLAIN_INLINE.search(code[code.index("VMAF_SYCL_ALWAYS_INLINE") :]):
        failures.append(f"{MATH}: a kernel function is not always inlined")
    if PLAIN_INLINE.search(_code(soft)):
        failures.append(f"{SOFT}: a kernel function is not always inlined")
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
        _kernel_failures(sources[TWIN])
        + _host_failures(sources[TWIN])
        + _math_failures(sources[MATH], sources[SOFT])
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


class IntegerSsimSyclExactContract(unittest.TestCase):
    def _assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_group_reduction_is_detected(self) -> None:
        # The pre-ADR-1443 store_integer_group().
        failures = _appended(
            TWIN,
            "\nstatic long r(sycl::nd_item<2> item, long score) {"
            " return sycl::reduce_over_group(item.get_group(), score, sycl::plus<long>{}); }\n",
        )
        self._assert_detected(failures, "reduced on the device")

    def test_float_term_is_detected(self) -> None:
        # The pre-ADR-1443 integer_ssim_contribution().
        failures = _appended(TWIN, "\nstatic float w(long weight) { return (float)weight; }\n")
        self._assert_detected(failures, "computes in float")

    def test_unstored_term_is_detected(self) -> None:
        failures = _replaced(TWIN, TERM_STORE, "const uint64_t term =")
        self._assert_detected(failures, "raster position")

    def test_required_sub_group_is_detected(self) -> None:
        # A required SIMD-16 spills on Xe-LP, SIMD-32 everywhere (ADR-1395).
        for size in (16, 32):
            failures = _replaced(TWIN, SHAPE_VALUES[0], f"constexpr int ISSIM_TERM_SG = {size};")
            self._assert_detected(failures, "sub-group size")

    def test_unflattened_term_is_detected(self) -> None:
        failures = _replaced(
            TWIN,
            "__attribute__((flatten, always_inline)) static inline uint64_t",
            "static inline uint64_t",
        )
        self._assert_detected(failures, "not flattened")

    def test_host_sum_of_a_part_is_detected(self) -> None:
        failures = _replaced(TWIN, HOST_CALL, "frame_sum_of_terms(s->h_terms, s->width);")
        self._assert_detected(failures, "whole term plane")

    def test_reordered_host_sum_is_detected(self) -> None:
        failures = _replaced(
            TWIN,
            "for (size_t i = 0U; i < count; i++) {\n        sum +=",
            "for (size_t i = count; i-- > 0U;) {\n        sum +=",
        )
        self._assert_detected(failures, "raster order")

    def test_regrouped_numerator_is_detected(self) -> None:
        # w * (a * b) rounds elsewhere than (w * a) * b on an edge window.
        failures = _replaced(
            MATH,
            "means = times_weight(signed_add(sums.twice_mxy, c1), m.w);",
            "means = signed_add(sums.twice_mxy, c1);",
        )
        self._assert_detected(failures, "starts with w * (2 * mxy + c1)")

    def test_squared_weight_is_detected(self) -> None:
        # k * (w * w) is one rounding where the reference has two.
        failures = _replaced(
            MATH,
            "c1 = times_weight(times_weight(k.k1, m.w), m.w);",
            "c1 = signed_mul(k.k1, signed_from_u64(m.w * m.w));",
        )
        self._assert_detected(failures, "c1 is ((k1 * w) * w)")

    def test_regrouped_variances_are_detected(self) -> None:
        failures = _replaced(
            MATH,
            "both = signed_add(reference_variance, signed_from_u64(p.y2w));",
            "both = signed_add(reference_variance, signed_sub(signed_from_u64(p.y2w), my2));",
        )
        self._assert_detected(failures, "joins before my2 leaves")

    def test_wider_integer_path_is_detected(self) -> None:
        # At 2^53 a sum of two products is no longer an fp64 value.
        failures = _replaced(MATH, "uint64_t{1} << 52;", "uint64_t{1} << 53;")
        self._assert_detected(failures, "stops at 2^52")

    def test_float_in_the_term_is_detected(self) -> None:
        failures = _appended(MATH, "\ninline constexpr float kHalf = 0.5f;\n")
        self._assert_detected(failures, "floating-point type")

    def test_changed_reference_is_detected(self) -> None:
        failures = _replaced(
            REFERENCE,
            "c1 = sm * sm * SSIM_K1 * w_d * w_d;",
            "c1 = sm * sm * SSIM_K1 * (w_d * w_d);",
        )
        self._assert_detected(failures, "the twin mirrors it")


if __name__ == "__main__":
    unittest.main()
