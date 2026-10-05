#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the bit-exact ssimulacra2_sycl sums (ADR-1446).

The CPU extractor (``ssimulacra2.c::ssim_map()`` and ``::edge_diff_map()``)
forms six fp64 terms per sample and channel and adds each into one
``double``, pixel after pixel. A SYCL kernel has no fp64 type (ADR-0220), so
``ssimulacra2_sycl``:

- forms each term as the reference's double in 64-bit integers
  (``sycl_ssimulacra2_math.h`` on ``sycl_soft_signed.h``), the reference's
  operations one for one, from the fp32 values the reference converts;
- returns the bits of the reference's loop over the terms
  (``sycl_ordered_sum.h`` on ``ordered_sum.h``, ADR-1433): integer increments
  per chunk of consecutive pixels, composed in pixel order, and one walk per
  sum that adds a chunk from its increment only where that is the loop's
  result and term by term everywhere else;
- uses its fp32 pair sums as advice for the plan only, never as a result.

Device-free: reads the sources only. Every planted regression below is a
construct the pre-ADR-1446 twin had, an order the CPU does not use, or an
operation that rounds elsewhere, so the contract fails on the old design and
passes on the new one. ``test_sycl_ssimulacra2_math`` checks the terms and
``test_sycl_ordered_sum`` the sum against the reference's expressions, on the
host and on a device; ``test_sycl_ssimulacra2_parity`` checks the scores.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"

TWIN = "sycl/ssimulacra2_sycl.cpp"
MATH = "sycl/sycl_ssimulacra2_math.h"
SUM = "sycl/sycl_ordered_sum.h"
SOFT = "sycl/sycl_soft_signed.h"
SHARED = "ordered_sum.h"
REFERENCE = "ssimulacra2.c"
MATH_TEST = ROOT / "core" / "test" / "test_sycl_ssimulacra2_math.c"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
DOUBLE_TYPE = re.compile(r"\bdouble\b")
PLAIN_INLINE = re.compile(r"^inline\s+(?!constexpr)", re.M)
NO_FP64_GUARD = "#ifndef VMAF_ORDSUM_NO_FP64"
NO_FP64_MACRO = "VMAF_ORDSUM_NO_FP64"
ADVICE_NAME = re.compile(r"(?<!\w)chunk_sums\b")

# The terms the sums take are the exact ones, staged under the chunk's plan.
EXACT_TERMS = (
    "const vmaf_sycl_ss2::TermPair d = ss2s_exact_ssim(a_, (size_t)at.c * a_.plane + i);",
    "ss2s_stage_units(a_, at, 0u, d.value, mine);",
    "ss2s_stage_units(a_, at, 1u, d.fourth, mine + 2);",
    "const vmaf_sycl_ss2::EdgeTerms e = ss2s_exact_edge(a_, (size_t)at.c * a_.plane + i);",
    "ss2s_stage_units(a_, at, 2u, e.artifact.value, mine);",
    "ss2s_stage_units(a_, at, 3u, e.artifact.fourth, mine + 2);",
    "ss2s_stage_units(a_, at, 4u, e.detail.value, mine + 4);",
    "ss2s_stage_units(a_, at, 5u, e.detail.fourth, mine + 6);",
)
EXACT_INPUTS = (
    "return vmaf_sycl_ss2::ssim_terms(in.num_m, in.num_s, in.denom_s);",
    "return vmaf_sycl_ss2::edge_terms(a.img1[idx], a.mu1[idx], a.img2[idx], a.mu2[idx]);",
)
# A lane's pixels are consecutive, and so are the lanes of a chunk.
LANE_PIXEL = ".pixel = (size_t)chunk * SS2S_CHUNK + (size_t)lane * SS2S_LANE_PIXELS};"
LANE_LOOP = "for (size_t i = at.pixel; i < at.pixel + SS2S_LANE_PIXELS && i < a_.plane; i++) {"
# Lanes are composed with their neighbour, the earlier pixels on the left.
ADJACENT_PAIR = (
    "for (unsigned stride = 1u; stride < RUN; stride <<= 1u) {",
    "if ((lane & (2u * stride - 1u)) == 0u) {",
    "const int64_t *next = lds + (size_t)(lane + stride) * SUMS * 2u;",
    "vmaf_ordsum_then(vmaf_ordsum_units(mine[s], mine[s + 1u]),",
    "vmaf_ordsum_units(next[s], next[s + 1u]));",
)
PLANNED_TERM = "vmaf_ordsum_planned_term_bits(bits, vmaf_sycl_ordsum::plan_of(entry)));"
# One walk per sum, on one lane, with the exact terms behind it.
WALK = (
    "if (it.get_local_id(0) != 0u)",
    "const uint64_t sum = vmaf_sycl_ordsum::walk_sum(",
    "walk, [&a, k, offset](size_t i) { return ss2s_term_bits(a, k, offset + i); });",
    "a_.totals[which] = sum;",
)
READBACK = "q.memcpy(s->h_totals, s->d_totals, SS2S_TOTALS * sizeof(uint64_t));"
HOST_SUMS = "sum[k] = std::bit_cast<double>(totals[(size_t)c * SS2S_SUMS + k]);"
# The advice sums reach the plan and nothing else.
ADVICE_READ = "args.chunk_sums + (size_t)c * args.chunks * SS2S_SUMS + k;"
ADVICE_USES = 4  # the struct member, the store, the plan's read, the frame chain
# Measured on an Arc A380: wider sub-groups spill registers, and the slot
# kernel does at the default register file (scratch memory, ADR-1395). On
# Xe-LP, which has no large register file, the slot kernel spills at a
# required SIMD-16 (UHD 770): its size is left to the compiler (ADR-1501).
SHAPES = (
    "constexpr int SS2S_UNITS_SG = 16;",
    "constexpr int SS2S_UNITS_GRF = 0;",
    "constexpr int SS2S_SLOT_SG = 0;",
    "constexpr int SS2S_SLOT_GRF = 256;",
    "constexpr int SS2S_WALK_SG = 16;",
    "constexpr int SS2S_WALK_GRF = 0;",
)
SHAPED_KERNELS = (
    "class Ss2SsimUnitsKernel : public VmafSyclKernelShape<SS2S_UNITS_SG, SS2S_UNITS_GRF>",
    "class Ss2EdgeUnitsKernel : public VmafSyclKernelShape<SS2S_UNITS_SG, SS2S_UNITS_GRF>",
    "class Ss2SlotKernel : public VmafSyclKernelShape<SS2S_SLOT_SG, SS2S_SLOT_GRF>",
    "class Ss2TotalsKernel : public VmafSyclKernelShape<SS2S_WALK_SG, SS2S_WALK_GRF>",
)
OLD_TWIN = ("launch_combine_partials", "launch_combine_final", "d_partials")

# The reference's operations, in its order, as the math header writes them.
TERM_STEPS = {
    "the product of the two converted floats": (
        "product = signed_mul(signed_from_float(num_m), signed_from_float(num_s));"
    ),
    "one quotient by the converted denominator": (
        "ratio = signed_div(product, signed_from_float(denom_s));"
    ),
    "d is 1.0 minus the quotient": "distance = signed_sub(kOne, ratio);",
    "quartic squares twice": "const SoftSigned square = signed_mul(x, x);",
    "quartic is the square of the square": "return signed_mul(square, square);",
    "an edge difference is |(double)a - (double)b|": (
        "return signed_abs(signed_sub(signed_from_float(a), signed_from_float(b)));"
    ),
    "the numerator is 1.0 + ed2": "numerator = signed_add(kOne, abs_difference(r2, m2));",
    "the denominator is 1.0 + ed1": "denominator = signed_add(kOne, abs_difference(r1, m1));",
    "d1 is the quotient minus 1.0": (
        "difference = signed_sub(signed_div(numerator, denominator), kOne);"
    ),
}

# What makes a plan advice: the sum decides, at its exact value.
SUM_STEPS = {
    "a chunk is added from its increment only where the plan holds": (
        "if (vmaf_ordsum_add_chunk_bits(&sum, plan_of(entry),"
    ),
    "a run is added under the binade the sum is in": (
        "added = vmaf_ordsum_add_chunk_bits(&sum, binade,"
    ),
    "a run's increments are the ones composed under that binade": (
        "const int64_t *units = runs + ((size_t)run * 2u + ahead) * 2u;"
    ),
    "a kept chunk's other terms are added one by one": (
        "sum = add_bits(sum, terms[(size_t)run * kRun + i]);"
    ),
    "a chunk without a slot has its terms computed in order": (
        "sum = add_bits(sum, term_of(i));"
    ),
    "an add that leaves the binade is the fp64 addition": (
        "return vmaf_sycl_soft::signed_bits(vmaf_sycl_soft::signed_add("
    ),
    "the shared header is used without its fp64 forms": "#define VMAF_ORDSUM_NO_FP64",
}

# The lines of ssimulacra2.c the header mirrors and the math test copies.
REFERENCE_LINES = (
    "double d = 1.0 - ((double)num_m * (double)num_s / (double)denom_s);",
    "if (d < 0.0)",
    "d = 0.0;",
    "double ed1 = fabs((double)r1[i] - (double)rm1[i]);",
    "double ed2 = fabs((double)r2[i] - (double)rm2[i]);",
    "double d1 = (1.0 + ed2) / (1.0 + ed1) - 1.0;",
    "vmaf_ss2_split_edge_difference(d1, &art, &det);",
)
# The loops whose bits the twin returns, and the fp32 inputs it recomputes.
REFERENCE_SUMS = (
    "sum_l1 += d;",
    "sum_l4 += quartic(d);",
    "s0 += art;",
    "s1 += quartic(art);",
    "s2 += det;",
    "s3 += quartic(det);",
    "float num_m = 1.0f - (mu1 - mu2) * (mu1 - mu2);",
    "float num_s = 2.0f * (rs12[i] - mu12) + kC2;",
    "float denom_s = (rs11[i] - mu11) + (rs22[i] - mu22) + kC2;",
)


def _code(source: str) -> str:
    """The source with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", source)


def _sources() -> dict[str, str]:
    names = (TWIN, MATH, SUM, SOFT, SHARED, REFERENCE)
    sources = {name: (FEATURE_ROOT / name).read_text(encoding="utf-8") for name in names}
    sources["test"] = MATH_TEST.read_text(encoding="utf-8")
    return sources


def _missing(name: str, code: str, pieces: tuple[str, ...], what: str) -> list[str]:
    return [f"{name}: {what} (`{piece}` is gone)" for piece in pieces if piece not in code]


def _twin_failures(twin: str) -> list[str]:
    code = _code(twin)
    failures = _missing(TWIN, code, EXACT_TERMS, "a sum no longer takes the exact term")
    failures += _missing(TWIN, code, EXACT_INPUTS, "the exact term has other inputs")
    failures += _missing(TWIN, code, (LANE_PIXEL, LANE_LOOP), "a lane's pixels are not in order")
    failures += _missing(TWIN, code, ADJACENT_PAIR, "the lanes are not composed in pixel order")
    failures += _missing(TWIN, code, (PLANNED_TERM,), "a term is not staged under its plan")
    failures += _missing(TWIN, code, WALK, "the totals are not one exact walk per sum")
    failures += _missing(TWIN, code, (READBACK, HOST_SUMS), "the sums are not read as fp64 bits")
    failures += _missing(TWIN, code, SHAPES + SHAPED_KERNELS, "a kernel shape changed")
    if ADVICE_READ not in code or len(ADVICE_NAME.findall(code)) != ADVICE_USES:
        failures.append(f"{TWIN}: the fp32 advice sums are read outside the plan")
    for name in OLD_TWIN:
        if name in code:
            failures.append(f"{TWIN}: the fp32 pair tree is back ({name})")
    return failures


def _fp64_outside_guard(shared: str) -> bool:
    """True when ordered_sum.h names `double` outside its no-fp64 guards."""
    depth = 0
    guarded = 0
    for line in _code(shared).splitlines():
        stripped = line.strip()
        if stripped.startswith(("#if", "#ifdef", "#ifndef")):
            depth += 1
            if NO_FP64_MACRO in stripped and guarded == 0:
                guarded = depth
        elif stripped.startswith("#endif"):
            if depth == guarded:
                guarded = 0
            depth -= 1
        elif guarded == 0 and DOUBLE_TYPE.search(line):
            return True
    return False


def _header_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    math = _code(sources[MATH])
    summed = _code(sources[SUM])
    for what, line in TERM_STEPS.items():
        if line not in math:
            failures.append(f"{MATH}: {what}: `{line}` is gone")
    for what, line in SUM_STEPS.items():
        if line not in summed:
            failures.append(f"{SUM}: {what}: `{line}` is gone")
    for name, code in ((MATH, math), (SUM, summed), (SOFT, _code(sources[SOFT]))):
        if DOUBLE_TYPE.search(code):
            failures.append(f"{name}: a kernel header names the fp64 type")
        if PLAIN_INLINE.search(code):
            failures.append(f"{name}: a kernel function is not always inlined")
    if _fp64_outside_guard(sources[SHARED]):
        failures.append(f"{SHARED}: an fp64 form is outside `{NO_FP64_GUARD}`")
    return failures


def _reference_failures(reference: str, test: str) -> list[str]:
    failures: list[str] = []
    for line in REFERENCE_LINES:
        if line not in reference:
            failures.append(f"{REFERENCE}: no longer holds `{line}`; the twin mirrors it")
        if line not in test:
            failures.append(f"{MATH_TEST.name}: reference_terms() lost `{line}`")
    for line in REFERENCE_SUMS:
        if line not in reference:
            failures.append(f"{REFERENCE}: no longer holds `{line}`; the twin mirrors it")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _twin_failures(sources[TWIN])
        + _header_failures(sources)
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


class Ssimulacra2SyclExactContract(unittest.TestCase):
    def _assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_advice_term_in_a_sum_is_detected(self) -> None:
        # The pre-ADR-1446 twin added the fp32 pair's value.
        failures = _replaced(
            TWIN,
            "ss2s_stage_units(a_, at, 0u, d.value, mine);",
            "ss2s_stage_units(a_, at, 0u, advice_bits(a_, at), mine);",
        )
        self._assert_detected(failures, "no longer takes the exact term")

    def test_advice_sum_as_a_result_is_detected(self) -> None:
        failures = _appended(
            TWIN, "\nstatic float t(const Ss2CombineArgs &a) { return a.chunk_sums[0]; }\n"
        )
        self._assert_detected(failures, "read outside the plan")

    def test_pair_tree_is_detected(self) -> None:
        failures = _appended(
            TWIN, "\nvoid launch_combine_final(sycl::queue &q, const Ss2FinalArgs &args);\n"
        )
        self._assert_detected(failures, "pair tree is back")

    def test_reversed_composition_is_detected(self) -> None:
        # vmaf_ordsum_then() is not commutative: later pixels on the left is
        # another order of the loop.
        failures = _replaced(
            TWIN,
            "vmaf_ordsum_then(vmaf_ordsum_units(mine[s], mine[s + 1u]),\n"
            "                                     vmaf_ordsum_units(next[s], next[s + 1u]));",
            "vmaf_ordsum_then(vmaf_ordsum_units(next[s], next[s + 1u]),\n"
            "                                     vmaf_ordsum_units(mine[s], mine[s + 1u]));",
        )
        self._assert_detected(failures, "not composed in pixel order")

    def test_halving_tree_is_detected(self) -> None:
        # The fp32 advice tree's shape pairs lane i with lane i + stride from
        # the top down, which interleaves the pixels.
        failures = _replaced(
            TWIN,
            "if ((lane & (2u * stride - 1u)) == 0u) {",
            "if (lane < stride) {",
        )
        self._assert_detected(failures, "not composed in pixel order")

    def test_strided_lane_is_detected(self) -> None:
        failures = _replaced(TWIN, LANE_PIXEL, ".pixel = (size_t)chunk * SS2S_CHUNK + lane};")
        self._assert_detected(failures, "pixels are not in order")

    def test_unplanned_term_is_detected(self) -> None:
        failures = _replaced(
            TWIN,
            "vmaf_ordsum_planned_term_bits(bits, vmaf_sycl_ordsum::plan_of(entry)));",
            "vmaf_ordsum_term_bits(bits, 0)));",
        )
        self._assert_detected(failures, "not staged under its plan")

    def test_walk_on_every_lane_is_detected(self) -> None:
        failures = _replaced(TWIN, "if (it.get_local_id(0) != 0u)\n            return;\n", "")
        self._assert_detected(failures, "one exact walk per sum")

    def test_float_totals_are_detected(self) -> None:
        # The pre-ADR-1446 read-back: pairs of floats per sum.
        failures = _replaced(
            TWIN,
            "SS2S_TOTALS * sizeof(uint64_t));\n}",
            "SS2S_TOTALS * 2u * sizeof(float));\n}",
        )
        self._assert_detected(failures, "not read as fp64 bits")

    def test_wider_sub_group_is_detected(self) -> None:
        failures = _replaced(TWIN, SHAPES[0], "constexpr int SS2S_UNITS_SG = 32;")
        self._assert_detected(failures, "kernel shape changed")

    def test_required_slot_sub_group_is_detected(self) -> None:
        failures = _replaced(TWIN, SHAPES[2], "constexpr int SS2S_SLOT_SG = 16;")
        self._assert_detected(failures, "kernel shape changed")

    def test_small_register_file_is_detected(self) -> None:
        failures = _replaced(TWIN, SHAPES[3], "constexpr int SS2S_SLOT_GRF = 0;")
        self._assert_detected(failures, "kernel shape changed")

    def test_float_product_is_detected(self) -> None:
        # (double)(num_m * num_s) rounds the product to fp32 first.
        failures = _replaced(
            MATH,
            "product = signed_mul(signed_from_float(num_m), signed_from_float(num_s));",
            "product = signed_from_float(num_m * num_s);",
        )
        self._assert_detected(failures, "product of the two converted floats")

    def test_reciprocal_quotient_is_detected(self) -> None:
        failures = _replaced(
            MATH,
            "ratio = signed_div(product, signed_from_float(denom_s));",
            "ratio = signed_mul(product, signed_from_float(1.0f / denom_s));",
        )
        self._assert_detected(failures, "one quotient by the converted denominator")

    def test_fused_edge_difference_is_detected(self) -> None:
        # (ed2 - ed1) / (1 + ed1) is the same value before rounding.
        failures = _replaced(
            MATH,
            "difference = signed_sub(signed_div(numerator, denominator), kOne);",
            "difference = signed_div(signed_sub(numerator, denominator), denominator);",
        )
        self._assert_detected(failures, "d1 is the quotient minus 1.0")

    def test_cubed_quartic_is_detected(self) -> None:
        failures = _replaced(
            MATH,
            "return signed_mul(square, square);",
            "return signed_mul(signed_mul(square, x), x);",
        )
        self._assert_detected(failures, "square of the square")

    def test_unchecked_chunk_is_detected(self) -> None:
        # Adding a chunk's increment without the check at the exact sum is
        # the loop's result only while the plan happens to be right.
        failures = _replaced(
            SUM,
            "if (vmaf_ordsum_add_chunk_bits(&sum, plan_of(entry),",
            "if (add_unchecked(&sum, plan_of(entry),",
        )
        self._assert_detected(failures, "only where the plan holds")

    def test_run_under_the_expected_binade_is_detected(self) -> None:
        failures = _replaced(
            SUM,
            "const int64_t *units = runs + ((size_t)run * 2u + ahead) * 2u;",
            "const int64_t *units = runs + (size_t)run * 4u;",
        )
        self._assert_detected(failures, "composed under that binade")

    def test_fp64_in_a_kernel_header_is_detected(self) -> None:
        failures = _appended(SUM, "\ninline constexpr double kHalf = 0.5;\n")
        self._assert_detected(failures, "names the fp64 type")

    def test_unguarded_fp64_form_is_detected(self) -> None:
        failures = _replaced(SHARED, NO_FP64_GUARD, "#ifndef VMAF_ORDSUM_SOMETHING_ELSE")
        self._assert_detected(failures, "is outside")

    def test_plain_inline_is_detected(self) -> None:
        failures = _appended(MATH, "\ninline int helper(int x) { return x; }\n")
        self._assert_detected(failures, "not always inlined")

    def test_changed_reference_is_detected(self) -> None:
        failures = _replaced(
            REFERENCE,
            "double d1 = (1.0 + ed2) / (1.0 + ed1) - 1.0;",
            "double d1 = (ed2 - ed1) / (1.0 + ed1);",
        )
        self._assert_detected(failures, "the twin mirrors it")


if __name__ == "__main__":
    unittest.main()
