#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the bit-exact ssimulacra2_hip sums (ADR-1445).

The CPU extractor (``ssimulacra2.c::ssim_map()`` and ``::edge_diff_map()``)
evaluates six terms per pixel and channel in ``double`` and adds each into one
``double``, pixel after pixel. Before ADR-1445 the HIP twin evaluated the
terms as pairs of floats and added them in a fixed tree: it equalled the CPU
on no measured frame of real content and was up to 7.6e-11 from it.

It now evaluates the CPU's fp64 expressions and returns the bits of the CPU's
loops with ``feature/ordered_sum.h``, as the CUDA twin does (ADR-1433): per
chunk of pixels in raster order the terms become integer increments of the
binade the running sum is in, composed in pixel order, and one walk per sum
adds the chunks, falling back to the chunk's terms, one by one, where the sum
leaves its binade.

Device-free: reads the sources only. Every planted regression below is a way
back to arithmetic or an order the CPU does not use. ``test_ordered_sum``
checks the shared arithmetic on the host and ``test_hip_ssimulacra2_parity``
the scores on a device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HIP_ROOT = ROOT / "core" / "src" / "feature" / "hip"

HOST = "ssimulacra2_hip.c"
KERNEL = "ssimulacra2/ssimulacra2_device.hip"
HEADER = "ssimulacra2_hip.h"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)

KERNELS = (
    "ssimulacra2_chunk_sums",
    "ssimulacra2_chunk_plan",
    "ssimulacra2_chunk_units",
    "ssimulacra2_ordered_totals",
)
OLD_KERNELS = ("ssimulacra2_combine_partials", "ssimulacra2_combine_final")
SHARED_HELPERS = '#include "feature/ordered_sum.h"'
# The CPU's fp64 expressions for the two terms the others derive from.
FP64_TERMS = (
    "double d = 1.0 - ((double)num_m * (double)num_s / (double)denom_s);",
    "const double ed1 = fabs((double)a.img1[idx] - (double)mu1);",
    "const double ed2 = fabs((double)a.img2[idx] - (double)mu2);",
    "const double d1 = (1.0 + ed2) / (1.0 + ed1) - 1.0;",
    "vmaf_ss2_split_edge_difference(d1, &artifact, &detail);",
)
# The pre-ADR-1445 pair arithmetic.
PAIR_ARITHMETIC = re.compile(r"\b(?:two_sum|two_prod|quick_two_sum|ff_add|ff_mul|ff_div)\s*\(")
CONTRACTION_OFF = "#pragma clang fp contract(off)"
# A lane's pixels are consecutive in raster order, and so are the lanes.
LANE_PIXEL = "const size_t i = (size_t)chunk * kChunkPixels + (size_t)lane * SS2H_CHUNK_RUN + j;"
# Lanes are composed with their neighbour, lower lane first.
ADJACENT_PAIR = (
    "if ((lane & (2u * step - 1u)) == 0u) {",
    "const VmafOrdsumUnits left = ss2h_units_load(shared, lane, k);",
    "const VmafOrdsumUnits right = ss2h_units_load(shared, lane + step, k);",
    "vmaf_ordsum_then(left, right)",
)
PLANNED_TERM = "vmaf_ordsum_then(units[k], vmaf_ordsum_planned_term(terms[k], plan));"
WALK_STEP = "vmaf_ordsum_add_chunk(sum, (int)plan[slot], u)"
# The fallback adds the chunk's terms in pixel order into the running sum.
TERM_SLOT = "terms_of_chunk[lane * SS2H_CHUNK_RUN + j] = terms[k];"
TERM_LOOP = (
    "for (size_t i = 0; i < kChunkPixels; i++) {",
    "state->sum += terms_of_chunk[i];",
)
TOTAL_STORE = "a.totals[(size_t)c * SS2H_SUMS + k] = state.sum;"
# The tree sum may only feed the plan.
TREE_STORE = "double *out = a.chunk_sums + ((size_t)c * a.chunks + chunk) * SS2H_SUMS;"
LAUNCHES = ("s->func_chunk_sums", "s->func_chunk_plan", "s->func_chunk_units", "s->func_totals")


def _code(source: str) -> str:
    """The source with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", source)


def _flat(source: str) -> str:
    """Code with every run of whitespace collapsed, so line breaks do not matter."""
    return " ".join(_code(source).split())


def _sources() -> dict[str, str]:
    return {name: (HIP_ROOT / name).read_text(encoding="utf-8") for name in (HOST, KERNEL, HEADER)}


def _term_failures(kernel: str) -> list[str]:
    failures: list[str] = []
    code = _flat(kernel)
    for piece in FP64_TERMS:
        if piece not in code:
            failures.append(f"{KERNEL}: a term is not the CPU's fp64 expression ({piece})")
    if PAIR_ARITHMETIC.search(code):
        failures.append(f"{KERNEL}: a term is evaluated in fp32 pairs")
    if CONTRACTION_OFF not in kernel:
        failures.append(f"{KERNEL}: the module no longer turns contraction off")
    return failures


def _sum_failures(kernel: str) -> list[str]:
    failures: list[str] = []
    code = _flat(kernel)
    for name in KERNELS:
        if f" void {name}(Ss2hCombineArgs a)" not in code:
            failures.append(f"{KERNEL}: kernel {name} is missing")
    failures.extend(
        f"{KERNEL}: the tree reduction {name} is back" for name in OLD_KERNELS if name in code
    )
    if SHARED_HELPERS not in kernel:
        failures.append(f"{KERNEL}: the ordered-sum helpers are not the shared header's")
    if LANE_PIXEL not in code:
        failures.append(f"{KERNEL}: a lane's pixels are not consecutive in raster order")
    if any(piece not in code for piece in ADJACENT_PAIR):
        failures.append(f"{KERNEL}: the lanes are not composed in lane order")
    if PLANNED_TERM not in code:
        failures.append(f"{KERNEL}: a lane does not compose its terms in pixel order")
    if WALK_STEP not in code:
        failures.append(f"{KERNEL}: the walk does not check a chunk against the exact sum")
    if TERM_SLOT not in code or any(piece not in code for piece in TERM_LOOP):
        failures.append(f"{KERNEL}: the fallback does not add the terms in pixel order")
    if TOTAL_STORE not in code:
        failures.append(f"{KERNEL}: the stored total is not the walked sum")
    if code.count("a.totals[") != 1:
        failures.append(f"{KERNEL}: more than one kernel writes the totals")
    if TREE_STORE not in code:
        failures.append(f"{KERNEL}: the tree sums are not the plan's input")
    return failures


def _host_failures(host: str) -> list[str]:
    failures: list[str] = []
    code = _flat(host)
    positions = [code.find(f"ss2h_launch(s, {launch},") for launch in LAUNCHES]
    if -1 in positions or positions != sorted(positions):
        failures.append(f"{HOST}: the four launches of the sums are not all there, in order")
    failures.extend(f"{HOST}: the host still loads {name}" for name in OLD_KERNELS if name in code)
    if "SS2H_NUM_SCALES * SS2H_TOTALS_PER_SCALE * sizeof(double)" not in code:
        failures.append(f"{HOST}: the readback is no longer the per-scale totals in double")
    if "const double *sum = totals + (size_t)c * SS2H_SUMS;" not in code:
        failures.append(f"{HOST}: collect() no longer reads the device's sums as they are")
    return failures


def _header_failures(header: str) -> list[str]:
    code = _flat(header)
    if "#define SS2H_CHUNK_PIXELS (SS2H_REDUCE_BLOCK * SS2H_CHUNK_RUN)" not in code:
        return [f"{HEADER}: a chunk is not one work-group of lanes times a run"]
    return []


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _term_failures(sources[KERNEL])
        + _sum_failures(sources[KERNEL])
        + _host_failures(sources[HOST])
        + _header_failures(sources[HEADER])
    )


class Ssimulacra2HipExactContract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _contract_failures(sources)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_pair_term_is_detected(self) -> None:
        # The pre-ADR-1445 ss2h_ssim_term().
        sources = _sources()
        sources[
            KERNEL
        ] += "\nFf t(float a, float b) { return ff_div(two_prod(a, b), Ff{b, 0.0f}); }\n"
        self.assertTrue(any("fp32 pairs" in item for item in _contract_failures(sources)))

    def test_fp32_term_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "    double d = 1.0 - ((double)num_m * (double)num_s / (double)denom_s);",
            "    double d = (double)(1.0f - (num_m * num_s / denom_s));",
        )
        self.assertTrue(any("fp64 expression" in item for item in failures), failures)

    def test_contraction_is_detected(self) -> None:
        failures = self._edited(KERNEL, CONTRACTION_OFF, "")
        self.assertTrue(any("contraction off" in item for item in failures), failures)

    def test_tree_total_is_detected(self) -> None:
        # The pre-ADR-1445 design: the group tree's result stored as the total.
        sources = _sources()
        sources[KERNEL] += "\nvoid f(Ss2hCombineArgs a) { a.totals[0] = 0.0; }\n"
        failures = _contract_failures(sources)
        self.assertTrue(any("more than one kernel writes the totals" in item for item in failures))

    def test_old_reduction_kernels_are_detected(self) -> None:
        sources = _sources()
        sources[KERNEL] += "\nvoid ssimulacra2_combine_final(void) {}\n"
        sources[HOST] += '\nstatic const char *n = "ssimulacra2_combine_partials";\n'
        failures = _contract_failures(sources)
        self.assertTrue(any("tree reduction" in item for item in failures))
        self.assertTrue(any("still loads" in item for item in failures))

    def test_strided_lanes_are_detected(self) -> None:
        # The old kernel's lanes took a strided subset of the plane.
        failures = self._edited(
            KERNEL, "(size_t)lane * SS2H_CHUNK_RUN + j;", "(size_t)lane + (size_t)j * 256u;"
        )
        self.assertTrue(any("consecutive" in item for item in failures), failures)

    def test_halving_tree_over_increments_is_detected(self) -> None:
        # Pairing lane with lane + half composes out of order.
        failures = self._edited(
            KERNEL,
            "ss2h_units_load(shared, lane + step, k);",
            "ss2h_units_load(shared, lane ^ step, k);",
        )
        self.assertTrue(any("lane order" in item for item in failures), failures)

    def test_unchecked_walk_is_detected(self) -> None:
        failures = self._edited(
            KERNEL, "if (!vmaf_ordsum_add_chunk(sum, (int)plan[slot], u)) {", "if (u.even < 0) {"
        )
        self.assertTrue(any("exact sum" in item for item in failures), failures)

    def test_reordered_fallback_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "for (size_t i = 0; i < kChunkPixels; i++) {",
            "for (size_t i = kChunkPixels; i-- > 0u;) {",
        )
        self.assertTrue(any("pixel order" in item for item in failures), failures)

    def test_missing_launch_is_detected(self) -> None:
        failures = self._edited(
            HOST, "ss2h_launch(s, s->func_chunk_units,", "ss2h_launch(s, s->func_chunk_sums,"
        )
        self.assertTrue(any("four launches" in item for item in failures), failures)

    def test_pair_readback_is_detected(self) -> None:
        # The pre-ADR-1445 collect(): the high and low float of a pair, added.
        failures = self._edited(
            HOST,
            "        const double *sum = totals + (size_t)c * SS2H_SUMS;",
            "        const double sum[SS2H_SUMS] = {(double)((const float *)totals)[c]};",
        )
        self.assertTrue(any("as they are" in item for item in failures), failures)


if __name__ == "__main__":
    unittest.main()
