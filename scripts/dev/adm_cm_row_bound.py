#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Largest integer-ADM scale-0 contrast-masking row, by exact search.

The scale-0 masking reduction adds, per band row, the cube term
((x^2 + 2^28) >> 29) * x >> shift_cub of every column of the reduction region,
where x = |band| * weight when the reference is compared with itself (the
threshold is then 0 everywhere). This tool finds the largest such row the
library's own integer arithmetic can produce, for a picture width and a CSF
weight, and prints it as a fraction of INT64_MAX and of 2^64.

Model (docs/development/accumulator-bounds.md, "Integer ADM scale-0 rows"):
the four pixel rows that feed one band row hold, per column, either the
pattern (0, 0, max, 0) or (max, max, 0, max), which give the vertical
high-pass its largest value of either sign (+-27411 at 16 bits); the
horizontal low-pass then reads four columns at a step of two, so neighbouring
outputs share columns. A dynamic programme over the column signs finds the
pattern with the largest row sum. The decouple and the reciprocal table are
the CPU's (integer_adm_kernels.h) with the default gain limit of 100.

Usage::

    adm_cm_row_bound.py [--widths 32 64 15360] [--weights 36453 45000] [--band h|d]
    adm_cm_row_bound.py --pattern 64      # the column signs of the worst 64-px row
"""

from __future__ import annotations

import argparse
import itertools
import math
import sys
from collections.abc import Sequence

LO = (15826, 27411, 7345, -4240)
HI = (-4240, -7345, 27411, -15826)
V_HI_16BIT = 27411  # vertical high-pass of (0, 0, 65535, 0) at 16 bits
INT64_MAX = 2**63 - 1
MIN_WIDTH = 17  # adm_frame_size_check(): the integer ADM extractor refuses less
DEFAULT_WEIGHT = {"h": 36453, "d": 49417}  # Watson97, 3H, 1080 (Q16)
BAND = {  # horizontal filter, cube shift offset, square shift, square rounding
    "h": (LO, 4, 29, 1 << 28),
    "d": (HI, 3, 30, 1 << 29),
}
GAIN_LIMIT = 100.0


def hpass(taps: Sequence[int], samples: Sequence[int]) -> int:
    """adm_dwt2_hpass(): four taps, rounded and shifted by 16."""
    return (sum(t * s for t, s in zip(taps, samples, strict=True)) + 32768) >> 16


def restored(o: int) -> int:
    """decouple_r of a sample compared with itself (t == o, angle flag set)."""
    if o == 0:
        return 0
    lut = (2**30) // abs(o) * (1 if o > 0 else -1)
    k = max(0, min(32768, (lut * o + 16384) >> 15))
    rst = (k * o + 16384) >> 15
    if rst > 0:
        return min(int(rst * GAIN_LIMIT), o)
    if rst < 0:
        return max(int(rst * GAIN_LIMIT), o)
    return rst


def cube_term(o: int, weight: int, band: str, shift_cub: int) -> int:
    """adm_cm_accum_round() with a zero threshold."""
    _, _, shift_sq, add_sq = BAND[band]
    v = abs(restored(o) * weight)
    v_sq = (v * v + add_sq) >> shift_sq
    add_cub = (1 << (shift_cub - 1)) if shift_cub > 0 else 0
    return (v_sq * v + add_cub) >> shift_cub


def region(width: int) -> range:
    """The band columns adm_cm_bounds() reduces (interior, border 10 %)."""
    band_w = (width + 1) // 2
    left = int(band_w * 0.1 - 0.5)
    return range(max(left, 0), min(band_w - left, band_w))


def best_row(width: int, weight: int, band: str) -> tuple[int, list[int]]:
    """(largest row sum, the column signs that reach it)."""
    taps, offset, _, _ = BAND[band]
    shift_cub = math.ceil(math.log2((width + 1) // 2) - offset)
    terms = {
        s: cube_term(hpass(taps, [x * V_HI_16BIT for x in s]), weight, band, shift_cub)
        for s in itertools.product((-1, 1), repeat=4)
    }
    columns = region(width)
    best: dict[tuple[int, int], tuple[int, list[int]]] = {}
    for s, term in terms.items():
        if term > best.get((s[2], s[3]), (-1, []))[0]:
            best[(s[2], s[3])] = (term, list(s))
    for _ in columns[1:]:
        step: dict[tuple[int, int], tuple[int, list[int]]] = {}
        for (a, b), (total, path) in best.items():
            for c, d in itertools.product((-1, 1), repeat=2):
                value = total + terms[(a, b, c, d)]
                if value > step.get((c, d), (-1, []))[0]:
                    step[(c, d)] = (value, [*path, c, d])
        best = step
    return max(best.values(), key=lambda item: item[0])


def pattern(width: int, band: str = "h") -> str:
    """The column signs of the worst row of `width`: '-' (max, max, 0, max), '+' the other."""
    _, path = best_row(width, DEFAULT_WEIGHT[band], band)
    first = 2 * region(width)[0] - 1
    signs = ["+"] * width
    for k, sign in enumerate(path):
        if 0 <= first + k < width:
            signs[first + k] = "+" if sign > 0 else "-"
    return "".join(signs)


def parse_args(argv: Sequence[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--widths", nargs="+", type=int, default=[32, 64, 1920, 8192, 15360, 32768])
    parser.add_argument("--weights", nargs="+", type=int, default=[])
    parser.add_argument("--band", choices=sorted(BAND), default="h")
    parser.add_argument("--pattern", type=int, default=0, metavar="WIDTH")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if args.pattern:
        print(pattern(args.pattern, args.band))
        return 0
    for weight in args.weights or [DEFAULT_WEIGHT[args.band]]:
        for width in args.widths:
            if width < MIN_WIDTH:
                continue
            total, _ = best_row(width, weight, args.band)
            print(
                f"band {args.band} weight {weight} width {width}: row {total} = "
                f"{total / INT64_MAX:.4f} INT64_MAX = {total / 2**64:.4f} of 2^64"
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())
