#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the bit-exact ssimulacra2_cuda sums (ADR-1433).

The CPU extractor (``ssimulacra2.c::ssim_map()`` and ``::edge_diff_map()``)
adds each of a channel's six per-pixel terms into one ``double``, pixel after
pixel. The twin computes the same terms, so the only way it can differ is
the order in which it adds them, and before ADR-1433 it added them in a fixed
tree: 102 of 110 measured frames were a few 1e-13 to 7e-11 from the CPU.

It now returns the bits of the CPU's loops with ``feature/ordered_sum.h``:
per chunk of pixels in raster order the terms become integer increments of
the binade the running sum is in, composed in pixel order, and one walk per
sum adds the chunks, falling back to the chunk's terms, one by one, where
the sum leaves its binade.

Device-free: reads the sources only. Every planted regression below is a way
back to an order the CPU does not use, so the contract fails on the old
design and passes on the new one. ``test_ordered_sum`` checks the arithmetic
on the host and ``test_cuda_ssimulacra2_parity`` the scores on a device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CUDA_ROOT = ROOT / "core" / "src" / "feature" / "cuda"

HOST = "ssimulacra2_cuda.c"
KERNEL = "ssimulacra2/ssimulacra2_device.cu"
HEADER = "ssimulacra2_cuda.h"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)

KERNELS = (
    "ssimulacra2_chunk_sums",
    "ssimulacra2_chunk_plan",
    "ssimulacra2_chunk_units",
    "ssimulacra2_ordered_totals",
)
OLD_KERNELS = ("ssimulacra2_combine_partials", "ssimulacra2_combine_final")
SHARED_HELPERS = '#include "feature/ordered_sum.h"'
# A lane's pixels are consecutive in raster order, and so are the lanes.
LANE_PIXEL = "const size_t i = (size_t)chunk * (size_t)SS2C_CHUNK_PIXELS + (size_t)lane * SS2C_CHUNK_RUN + j;"
# Lanes are composed with their neighbour, lower lane first.
ADJACENT_PAIR = (
    "if ((lane & (2u * step - 1u)) == 0u) {",
    "const VmafOrdsumUnits left = ss2c_units_load(shared, lane, k);",
    "const VmafOrdsumUnits right = ss2c_units_load(shared, lane + step, k);",
    "vmaf_ordsum_then(left, right)",
)
PLANNED_TERM = "vmaf_ordsum_then(units[k], vmaf_ordsum_planned_term(terms[k], plan));"
WALK_STEP = "vmaf_ordsum_add_chunk(sum, (int)plan[slot], u)"
# The fallback adds the chunk's terms in pixel order into the running sum.
TERM_SLOT = "terms_of_chunk[lane * SS2C_CHUNK_RUN + j] = terms[k];"
TERM_LOOP = (
    "for (const double term : terms_of_chunk)",
    "sum += term;",
)
TOTAL_STORE = "a.totals[(size_t)c * SS2C_SUMS + k] = sum;"
# The tree sum may only feed the plan.
TREE_STORE = "double *out = a.chunk_sums + ((size_t)c * a.chunks + chunk) * SS2C_SUMS;"
LAUNCHES = ("s->func_chunk_sums", "s->func_chunk_plan", "s->func_chunk_units", "s->func_totals")


def _code(source: str) -> str:
    """The source with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", source)


def _flat(source: str) -> str:
    """Code with every run of whitespace collapsed, so line breaks do not matter."""
    return " ".join(_code(source).split())


def _sources() -> dict[str, str]:
    return {name: (CUDA_ROOT / name).read_text(encoding="utf-8") for name in (HOST, KERNEL, HEADER)}


def _kernel_failures(kernel: str) -> list[str]:
    failures: list[str] = []
    code = _flat(kernel)
    for name in KERNELS:
        if f" {name}(const Ss2cCombineArgs a)" not in code:
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
    positions = [code.find(f"cuLaunchKernel({launch},") for launch in LAUNCHES]
    if -1 in positions or positions != sorted(positions):
        failures.append(f"{HOST}: the four launches of the sums are not all there, in order")
    failures.extend(f"{HOST}: the host still loads {name}" for name in OLD_KERNELS if name in code)
    if "h_totals" not in code or "SS2C_TOTALS_PER_SCALE * sizeof(double)" not in code:
        failures.append(f"{HOST}: the readback is no longer the per-scale totals")
    return failures


def _header_failures(header: str) -> list[str]:
    code = _flat(header)
    if "#define SS2C_CHUNK_PIXELS (SS2C_REDUCE_BLOCK * SS2C_CHUNK_RUN)" not in code:
        return [f"{HEADER}: a chunk is not one block of lanes times a run"]
    return []


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _kernel_failures(sources[KERNEL])
        + _host_failures(sources[HOST])
        + _header_failures(sources[HEADER])
    )


class Ssimulacra2CudaExactContract(unittest.TestCase):
    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_tree_total_is_detected(self) -> None:
        # The pre-ADR-1433 design: the block tree's result stored as the total.
        sources = _sources()
        sources[KERNEL] += "\nvoid f(const Ss2cCombineArgs a) { a.totals[0] = 0.0; }\n"
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
        sources = _sources()
        sources[KERNEL] = sources[KERNEL].replace(
            "(size_t)lane * SS2C_CHUNK_RUN + j;", "(size_t)lane + (size_t)j * 256u;", 1
        )
        self.assertTrue(any("consecutive" in item for item in _contract_failures(sources)))

    def test_halving_tree_over_increments_is_detected(self) -> None:
        # Pairing lane with lane + half composes out of order.
        sources = _sources()
        sources[KERNEL] = sources[KERNEL].replace(
            "ss2c_units_load(shared, lane + step, k);", "ss2c_units_load(shared, lane ^ step, k);"
        )
        self.assertTrue(any("lane order" in item for item in _contract_failures(sources)))

    def test_unchecked_walk_is_detected(self) -> None:
        sources = _sources()
        sources[KERNEL] = sources[KERNEL].replace(
            "if (!vmaf_ordsum_add_chunk(sum, (int)plan[slot], u)) {", "if (u.even < 0) {", 1
        )
        self.assertTrue(any("exact sum" in item for item in _contract_failures(sources)))

    def test_reordered_fallback_is_detected(self) -> None:
        sources = _sources()
        sources[KERNEL] = sources[KERNEL].replace(
            "for (const double term : terms_of_chunk)",
            "for (unsigned i = SS2C_CHUNK_PIXELS; i-- > 0u;)",
            1,
        )
        self.assertTrue(any("pixel order" in item for item in _contract_failures(sources)))

    def test_missing_launch_is_detected(self) -> None:
        sources = _sources()
        sources[HOST] = sources[HOST].replace(
            "cuLaunchKernel(s->func_chunk_units,", "cuLaunchKernel(s->func_chunk_sums,", 1
        )
        self.assertTrue(any("four launches" in item for item in _contract_failures(sources)))


if __name__ == "__main__":
    unittest.main()
