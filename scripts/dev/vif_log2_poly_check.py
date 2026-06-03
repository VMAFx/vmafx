#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Empirical bit-equivalence test: polynomial approximation vs the integer-VIF
log2 lookup table used in `core/src/feature/integer_vif.c`.

The table is generated as:

    log2_table[i] = (uint16_t) round( log2f((float)i) * 2048 )   for i in [32767, 65535]

The AVX-512 path performs three `vpgatherdq` lookups per 8-lane block against
this table (see `vif_avx512.c`). The post-merge CPU profile (see
`docs/research/0053-post-merge-cpu-profile-2026-05-03.md`) attributes 13% of
samples to the first gather alone. Replacing it with a 5th-order minimax
polynomial would eliminate the gather entirely and fold into VFMA-stride.

The fork's bit-exactness contract (ADR-0138 / ADR-0140) requires the SIMD
output to match the scalar reference exactly. Therefore the polynomial swap is
only legal if and only if the polynomial returns the **identical uint16
quantized value** at *every* integer input in the table's domain.

This script:

  1. Generates the reference table.
  2. Fits a 5th-order minimax polynomial in float32 / float64 to log2(x) on
     the same domain.
  3. Quantises the polynomial output the same way the table does
     (round-to-nearest-even, cast to uint16).
  4. Reports max-abs-diff, count of mismatched inputs, worst inputs.

Outcome interpretation:

  - max_abs_diff == 0 across all 32k entries => polynomial is bit-equivalent;
    AVX-512 swap is legal.
  - max_abs_diff > 0 even at one input => polynomial is approximate; swap
    violates bit-exactness; the optimisation is forbidden under the current
    contract.

The latter is the expected outcome: a 5-coefficient polynomial cannot
reproduce 32,768 independent integer outputs to the LSB.
"""

from __future__ import annotations

import numpy as np


def reference_table() -> np.ndarray:
    """Match `log_generate()` in core/src/feature/integer_vif.c."""
    table = np.zeros(65536, dtype=np.uint16)
    # Use float32 + roundf (banker's via numpy.rint after cast).
    idx = np.arange(32767, 65536, dtype=np.uint32)
    log2_f32 = np.log2(idx.astype(np.float32)).astype(np.float32)
    scaled = (log2_f32 * np.float32(2048.0)).astype(np.float32)
    # C round() is half-away-from-zero; values here are positive so floor(x+0.5)
    # is identical to round-half-away-from-zero.
    rounded = np.floor(scaled + np.float32(0.5)).astype(np.int64)
    table[32767:65536] = rounded.astype(np.uint16)
    return table


def fit_minimax_log2(degree: int, samples: int = 4096) -> np.ndarray:
    """Fit a polynomial p(x) ~= log2(x) on x in [32768, 65536].

    Uses Chebyshev least-squares on the same domain — minimax in the
    near-equioscillating sense. Good enough to bound max error.
    """
    x = np.linspace(32768.0, 65535.0, samples, dtype=np.float64)
    y = np.log2(x)
    # Normalise x to [-1, 1] for Chebyshev numerical conditioning.
    a, b = 32768.0, 65535.0
    t = (2.0 * x - (a + b)) / (b - a)
    return np.polynomial.chebyshev.Chebyshev.fit(t, y, deg=degree)


def evaluate_poly_table(cheb, domain_lo: int, domain_hi: int) -> np.ndarray:
    """Quantise the polynomial in the same way as `log_generate()`."""
    a, b = float(domain_lo), float(domain_hi - 1)
    idx = np.arange(domain_lo, domain_hi, dtype=np.uint32)
    t = (2.0 * idx.astype(np.float64) - (a + b)) / (b - a)
    poly_log2 = cheb(t).astype(np.float64)
    # Mirror the C: cast to float32, multiply by 2048 in float32, round.
    poly_f32 = poly_log2.astype(np.float32)
    scaled = (poly_f32 * np.float32(2048.0)).astype(np.float32)
    rounded = np.floor(scaled + np.float32(0.5)).astype(np.int64)
    return rounded.astype(np.uint16)


def report(degree: int) -> dict:
    table = reference_table()
    cheb = fit_minimax_log2(degree)
    poly_vals = evaluate_poly_table(cheb, 32768, 65536)
    ref_vals = table[32768:65536]
    diff = poly_vals.astype(np.int32) - ref_vals.astype(np.int32)
    abs_diff = np.abs(diff)
    max_abs = int(abs_diff.max())
    nonzero_count = int((abs_diff != 0).sum())
    total = abs_diff.size
    worst_idx = np.argsort(abs_diff)[-5:][::-1]
    worst_inputs = [
        (int(32768 + i), int(ref_vals[i]), int(poly_vals[i]), int(diff[i])) for i in worst_idx
    ]
    return {
        "degree": degree,
        "max_abs_diff": max_abs,
        "nonzero_count": nonzero_count,
        "total": total,
        "mismatch_pct": 100.0 * nonzero_count / total,
        "worst_inputs": worst_inputs,
    }


def main() -> None:
    print("Reference: log2_table[i] = uint16(round(log2f((float)i) * 2048))")
    print("Domain: i in [32768, 65535]  ->  32768 entries")
    print()
    for degree in (3, 4, 5, 6, 8, 10, 12):
        r = report(degree)
        print(
            f"degree={r['degree']:>2}  "
            f"max|diff|={r['max_abs_diff']:>6}  "
            f"mismatched={r['nonzero_count']:>5}/{r['total']} "
            f"({r['mismatch_pct']:.2f}%)"
        )
        if r["max_abs_diff"] > 0:
            print("             worst examples (input, ref, poly, diff):")
            for inp, ref, poly, d in r["worst_inputs"][:3]:
                print(f"               i={inp:>5}  ref={ref:>5}  " f"poly={poly:>5}  diff={d:+d}")
    print()
    print("Bit-equivalence requires max_abs_diff = 0 at every input.")
    print("Any non-zero diff violates ADR-0138 / ADR-0140 (SIMD must match scalar).")


if __name__ == "__main__":
    main()
