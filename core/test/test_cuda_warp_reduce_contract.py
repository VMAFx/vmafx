#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every lane of a warp reaches the CUDA warp reductions, and they shift no signed word.

T-CUDA-WARP-REDUCE-UB-2026-10-05. `warp_reduce()` and `warp_reduce_u64()` in
`core/src/cuda/cuda_helper.cuh` shuffle with the full mask, so every lane of
the warp must call them. The integer VIF horizontal kernel
(`vif_hori_kernel()` in `cuda/integer_vif/filter1d.cu`) called its flush
inside `if (y < h && x_start < w)`: at the right edge of the plane the lanes
past it did not take part in the shuffle, which is undefined. And
`warp_reduce(int64_t)` rebuilt each word as `(x & 0xffffffff) | (x >> 32) << 32`,
shifting a negative high word left, undefined before C++20.

Device-free: reads the two sources and reports both master forms (planted
below). The device side is `compute-sanitizer --tool synccheck` on the CUDA
VIF tests (docs/state.md row).
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "src"
HELPER = SRC / "cuda" / "cuda_helper.cuh"
FILTER1D = SRC / "feature" / "cuda" / "integer_vif" / "filter1d.cu"
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def block(text: str, start: int) -> tuple[int, int]:
    """(open, close) offsets of the brace block whose `{` is at or after `start`."""
    open_at = text.index("{", start)
    depth = 0
    for index in range(open_at, len(text)):
        depth += {"{": 1, "}": -1}.get(text[index], 0)
        if depth == 0:
            return open_at, index
    raise ValueError("unbalanced braces")


def function(text: str, name: str) -> str:
    """The comment-free definition of function `name`, or an empty string."""
    clean = COMMENT.sub(" ", text)
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", clean)
    if match is None:
        return ""
    _, close = block(clean, match.end() - 1)
    return clean[match.start() : close + 1]


def helper_problems(text: str) -> list[str]:
    """Why warp_reduce(int64_t) of `text` is not the unsigned reduction."""
    body = function(text, "warp_reduce")
    if not body:
        return ["warp_reduce() not found"]
    found = []
    if re.search(r"<<\s*32", body):
        found.append("warp_reduce() shifts a word left by 32")
    if "warp_reduce_u64(" not in body:
        found.append("warp_reduce() does not reduce through warp_reduce_u64()")
    return found


def kernel_problems(text: str) -> list[str]:
    """Why vif_hori_kernel() of `text` can reach its flush with part of a warp."""
    body = function(text, "vif_hori_kernel")
    if not body:
        return ["vif_hori_kernel() not found"]
    edge = re.search(r"if\s*\(\s*y\s*<\s*h\s*&&\s*x_start\s*<\s*w\s*\)", body)
    if edge is None:
        return ["vif_hori_kernel() has no `y < h && x_start < w` branch"]
    open_at, close = block(body, edge.end())
    flush = [m.start() for m in re.finditer(r"\bvif_hori_flush_accums\s*\(", body)]
    if not flush:
        return ["vif_hori_kernel() does not flush its accumulators"]
    if any(open_at < at < close for at in flush):
        return ["vif_hori_kernel() flushes inside the per-lane edge branch"]
    return []


# warp_reduce() and the end of vif_hori_kernel() as master had them (571565a47).
OLD_HELPER = """
__forceinline__ __device__ int64_t warp_reduce(int64_t x)
{
#pragma unroll
    for (int i = 16; i > 0; i >>= 1) {
        x += int64_t(__shfl_down_sync(0xffffffff, x & 0xffffffff, i)) |
             int64_t(__shfl_down_sync(0xffffffff, x >> 32, i) << 32);
    }
    return x;
}
"""
OLD_KERNEL = """
__device__ __forceinline__ void vif_hori_kernel(VifBufferCuda buf, int w, int h)
{
    if (y < h) {
        vif_hori_load_tile<TILE_W, use_ldg>(tile, buf, buf_row, tile_x0, w);
    }
    __syncthreads();
    if (y < h && x_start < w) {
        union {
            vif_accums thread_accum;
            int64_t thread_accum_i64[7] = {0};
        };
        vif_hori_statistics<val_per_thread>(sums, x_start, w, h, add_shift_round_HP, shift_HP,
                                            vif_enhn_gain_limit, thread_accum);
        vif_hori_flush_accums(thread_accum_i64, accum);
        vif_hori_store_rd<val_per_thread>(buf, sums, y, x_start, w, h);
    }
}
"""


class LiveSources(unittest.TestCase):
    def test_warp_reduce_shifts_no_signed_word(self) -> None:
        self.assertEqual(helper_problems(HELPER.read_text(encoding="utf-8")), [])

    def test_every_lane_reaches_the_vif_flush(self) -> None:
        self.assertEqual(kernel_problems(FILTER1D.read_text(encoding="utf-8")), [])


class PlantedRegressions(unittest.TestCase):
    def test_master_helper_is_reported(self) -> None:
        found = helper_problems(OLD_HELPER)
        self.assertIn("warp_reduce() shifts a word left by 32", found)
        self.assertIn("warp_reduce() does not reduce through warp_reduce_u64()", found)

    def test_master_kernel_is_reported(self) -> None:
        self.assertEqual(
            kernel_problems(OLD_KERNEL),
            ["vif_hori_kernel() flushes inside the per-lane edge branch"],
        )


if __name__ == "__main__":
    unittest.main()
