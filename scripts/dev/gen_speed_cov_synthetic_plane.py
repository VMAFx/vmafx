#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Write core/test/speed_cov_synthetic_plane.h, a synthetic SpEED covariance input.

The plane is 120 x 50 fp32 samples from splitmix64 (seed below), laid out
so that SpEED's covariance (5 x 5 block elements, 116 x 46 submatrix) has
entries whose terms cancel almost exactly after a long climb, the shape of
the cancelling blocks in speed_cov_cases.h:

- rows 2..23: pairs of rows, the second the first negated, magnitudes in
  [1, 2) with full significands, so a column adds to zero over each pair;
- row 24: magnitudes 2^-10 of that, so the element means stay near 1e-5;
- rows 25..47: rows 2..24 again with every odd column negated;
- rows 0, 1, 48, 49: samples in [-2, 2).

For block row 2 and a column lag of 1 or 3 (entries (11, 10), (12, 11),
(13, 12), (14, 13), (13, 10), (14, 11)) the terms of rows 2..24 add up to
thousands and those of rows 25..47 take the sum back to about 1e-5, so speed.c's
sequential fp64 sum (compute_cov_kernel_scalar(), every add rounded) and the
exact sum rounded once give different fp32 values: the failure class of
T-SPEED-CHROMA-SYCL-COV-1ULP-2026-10-06 (ADR-2690).

The script starts at --seed and takes the first seed whose plane has at least
--min-split such entries, then writes the header with that seed and the
entries. test_sycl_speed_cov_chain re-derives the entries with an x86-64
long double sum and fails when the plane has none, so a changed fixture cannot
lose the discriminator silently.

    python3 scripts/dev/gen_speed_cov_synthetic_plane.py            # rewrite
    python3 scripts/dev/gen_speed_cov_synthetic_plane.py --check    # compare
"""

from __future__ import annotations

import argparse
import struct
import sys
from fractions import Fraction
from pathlib import Path

WIDTH = 120
HEIGHT = 50
BLOCK = 5
SUB_W = WIDTH - BLOCK + 1
SUB_H = HEIGHT - BLOCK + 1
ELEMENTS = BLOCK * BLOCK
MASK = (1 << 64) - 1
TOP_FIRST = 2  # rows 2..24: the first half of the block-row-2 submatrices
TOP_LAST = 24
BOTTOM_FIRST = 25  # rows 25..47: rows 2..24 again, every odd column negated
OUT = Path(__file__).resolve().parents[2] / "core" / "test" / "speed_cov_synthetic_plane.h"


def f32(value: float) -> float:
    """value rounded to fp32. An fp32 add or divide done in fp64 and rounded
    here is the fp32 operation (53 >= 2 * 24 + 2)."""
    return float(struct.unpack("<f", struct.pack("<f", value))[0])


def bits32(value: float) -> int:
    return int(struct.unpack("<I", struct.pack("<f", value))[0])


def splitmix64(state: int) -> tuple[int, int]:
    state = (state + 0x9E3779B97F4A7C15) & MASK
    z = state
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK
    return state, z ^ (z >> 31)


def sample(state: int, low: float) -> tuple[int, float]:
    """(top 24 bits of the next splitmix64 value) / 2^22 + low: [low, low + 4)."""
    state, z = splitmix64(state)
    return state, (z >> 40) / float(1 << 22) + low


def plane(seed: int) -> list[float]:
    """The layout in the module docstring; every sample exact in fp32."""
    state = seed
    p = [0.0] * (WIDTH * HEIGHT)
    for r in (0, 1, 48, 49):
        for c in range(WIDTH):
            state, p[r * WIDTH + c] = sample(state, -2.0)
    for r in range(TOP_FIRST, TOP_LAST, 2):
        for c in range(WIDTH):
            state, u = sample(state, -2.0)
            u = abs(u) / 2.0 + 1.0  # [1, 2)
            u = f32(u)
            p[r * WIDTH + c] = u
            p[(r + 1) * WIDTH + c] = -u
    for c in range(WIDTH):
        state, u = sample(state, -2.0)
        p[TOP_LAST * WIDTH + c] = f32(abs(u) / 2.0 + 1.0) * 2.0**-10
    for k in range(TOP_LAST - TOP_FIRST + 1):
        for c in range(WIDTH):
            v = p[(TOP_FIRST + k) * WIDTH + c]
            p[(BOTTOM_FIRST + k) * WIDTH + c] = -v if c & 1 else v
    return p


def element(p: list[float], e: int) -> list[float]:
    r, c = divmod(e, BLOCK)
    return [p[(r + i) * WIDTH + c + j] for i in range(SUB_H) for j in range(SUB_W)]


def mean(values: list[float]) -> float:
    """speed.c compute_mean(): an fp32 running sum, an fp32 quotient."""
    total = 0.0
    for v in values:
        total = f32(total + v)
    return f32(total / float(SUB_W * SUB_H))


def entries(p: list[float]) -> list[tuple[int, int, int, int]]:
    """(x, y, cpu fp32 bits, exact fp32 bits) of every entry the two sums split."""
    means = [mean(element(p, e)) for e in range(ELEMENTS)]
    cols = [element(p, e) for e in range(ELEMENTS)]
    n = SUB_W * SUB_H
    found = []
    for x in range(ELEMENTS):
        dx = [v - means[x] for v in cols[x]]
        for y in range(x + 1):
            dy = [v - means[y] for v in cols[y]]
            terms = [a * b for a, b in zip(dx, dy, strict=True)]
            seq = 0.0
            for t in terms:
                seq += t
            cpu = bits32(seq / n)
            exact = bits32(float(sum((Fraction(t) for t in terms), Fraction(0)) / n))
            if cpu != exact:
                found.append((x, y, cpu, exact))
    return found


def c_hex(value: float) -> str:
    """The C99 hex literal of an fp32 value, without trailing zero digits."""
    text = value.hex()
    mantissa, exponent = text.split("p")
    if "." in mantissa:
        mantissa = mantissa.rstrip("0").rstrip(".")
    return f"{mantissa}p{exponent}f"


def render(seed: int, p: list[float], found: list[tuple[int, int, int, int]]) -> str:
    rows = []
    for i in range(0, len(p), 5):
        rows.append("    " + " ".join(f"{c_hex(v)}," for v in p[i : i + 5]))
    split = "\n".join(
        f" *    ({x}, {y}): sequential 0x{c:08x}, exact 0x{e:08x}" for x, y, c, e in found
    )
    return f"""/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  A synthetic SpEED covariance input: {WIDTH} x {HEIGHT} fp32 samples in raster
 *  order, drawn from splitmix64 seed 0x{seed:016x} in the layout described in
 *  scripts/dev/gen_speed_cov_synthetic_plane.py, which writes this file; do
 *  not edit. Rows 25..47 repeat rows 2..24 with every odd column negated, so
 *  the covariance terms of block row 2 at an odd column lag climb to
 *  thousands and cancel back to about 1e-5.
 *
 *  With 5 x 5 block elements (submatrix {SUB_W} x {SUB_H}) these entries of the
 *  covariance matrix are stored differently by speed.c's sequential fp64 sum
 *  and by the exact sum rounded once (T-SPEED-CHROMA-SYCL-COV-1ULP-2026-10-06):
{split}
 */

#ifndef VMAF_TEST_SPEED_COV_SYNTHETIC_PLANE_H_
#define VMAF_TEST_SPEED_COV_SYNTHETIC_PLANE_H_

enum {{
    SPEED_COV_SYNTHETIC_WIDTH = {WIDTH},
    SPEED_COV_SYNTHETIC_HEIGHT = {HEIGHT},
    SPEED_COV_SYNTHETIC_SPLIT = {len(found)},
}};

/* clang-format off */
static const float
    speed_cov_synthetic_plane[SPEED_COV_SYNTHETIC_WIDTH * SPEED_COV_SYNTHETIC_HEIGHT] = {{
{chr(10).join(rows)}
}};
/* clang-format on */

#endif /* VMAF_TEST_SPEED_COV_SYNTHETIC_PLANE_H_ */
"""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=0x5EED0C0F)
    ap.add_argument("--min-split", type=int, default=1)
    ap.add_argument("--tries", type=int, default=200)
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    for seed in range(args.seed, args.seed + args.tries):
        p = plane(seed)
        found = entries(p)
        print(f"seed 0x{seed:x}: {len(found)} split entries", file=sys.stderr)
        if len(found) >= args.min_split:
            text = render(seed, p, found)
            if args.check:
                same = OUT.exists() and OUT.read_text(encoding="utf-8") == text
                print("up to date" if same else f"{OUT} is stale", file=sys.stderr)
                return 0 if same else 1
            OUT.write_text(text, encoding="utf-8")
            print(f"wrote {OUT}", file=sys.stderr)
            return 0
    print("no seed in range has a split entry", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
