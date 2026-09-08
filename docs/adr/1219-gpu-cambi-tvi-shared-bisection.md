<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1219: The HIP and Metal CAMBI twins use the shared TVI bisection and the CPU's border rules

- **Status**: Proposed
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: `hip`, `metal`, `correctness`, `feature-extractor`, `testing`

## Context

CAMBI scores banding. Three parts of it are exact integer/threshold logic that a
GPU twin has to reproduce literally, and the HIP twin diverged on all three —
enough that its score **collapsed to exactly 0.0** on banding content that the
CPU scores at 5.85.

**1. The TVI table.** `get_tvi_for_diff()` bisects
`tvi_hard_threshold_condition` between `luma_range.foot` (64) and
`luma_range.head - diff - 1`, returning the largest sample at which
`delta_luminance > tvi_threshold * mean_luminance` still holds. The HIP and
Metal twins instead hand-rolled a binary search over the **negated** predicate,
seeded from luma 0 rather than `foot`, and derived `vlt_luma` as the *largest*
luma below the visibility threshold where the CPU takes the *smallest* luma at
or above it. Replicating both algorithms against the same luminance model at the
default `max_log_contrast = 2` gives

| `diff` | CPU bisection | hand-rolled |
| --- | --- | --- |
| 1 | 182 | 1026 |
| 2 | 309 | 1025 |
| 3 | 436 | 1024 |
| 4 | 563 | 4 |

Because `v_band_size = tvi_for_diff[num_diffs-1] + 1 - v_band_base`, the derived
luma band collapses from 564 entries to a handful and `calculate_c_values()`
discards almost every pixel as out-of-band.

**2. `filter_mode` border rows.** `cambi.c::filter_mode` runs the horizontal
pass into a 3-row ring buffer and writes the vertical result back only under
`if (i > 1)`, which covers output rows `1 .. height-2`. Rows `0` and `height-1`
therefore keep the **original, unfiltered** pixels — not the
horizontally-filtered ones, which never leave the ring. The CUDA and SYCL twins
already carry the matching `if (axis == 1 && (y == 0 || y >= height - 1)) return;`
guard; HIP and Metal filtered those rows.

**3. The 7x7 mask box sum.** `get_spatial_mask_for_index()` accumulates a
zero-padded summed-area table — `memset(dp, 0, ...)`,
`dp_width = width + 2*pad_size + 1`, and `deriv_valid = (i < height)` — so a tap
outside the frame contributes nothing. The HIP kernel **clamped** each tap to
the border pixel instead, counting that pixel's zero-derivative flag up to three
extra times per axis and flipping `box_sum > mask_index` on a band of border
pixels. CUDA and Metal already zero-pad.

Nothing caught any of this, because both the HIP and Metal CAMBI parity
fixtures were 8-bit gradients stepping 32 code levels every 32 columns. CAMBI
counts neighbour differences of `1 .. num_diffs` (4 at the default), so a
32-level step is an edge, not banding: **both fixtures scored exactly 0.0 on the
CPU as well**, and the parity assertion was `0 == 0`.

## Decision

We will delete the hand-rolled TVI search from the HIP and Metal twins and call
`vmaf_cambi_init_tvi_and_vlt()` — the CPU's own bisection, already used by the
SYCL twin — add the missing `filter_mode` vertical-border guard to both, and
make the HIP mask kernel zero-pad its box sum. Both parity fixtures become
10-bit banding gradients that score 5.846154 on the CPU, and both tests assert
the CPU score is non-degenerate before comparing, so the gate cannot rot back to
`0 == 0`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Call the shared `vmaf_cambi_init_tvi_and_vlt()` (chosen) | One implementation of the bisection for CPU, SYCL, HIP and Metal; cannot drift again; also fixes `vlt_luma` and validates the derived band | None — the helper is host-side scalar code the twins already run in `init()` | — |
| Port the CPU bisection into each twin | Keeps the twins self-contained | Re-creates exactly the duplication that produced this bug, in two more places | The CUDA twin's own hand-port is why this class exists |
| Fix the hand-rolled predicate in place | Smallest diff | Still a fourth copy of a subtle bisection, and it would not fix `vlt_luma` or the band validation | Same objection |
| Loosen the parity tolerance to accommodate the residual | Would have made the test green after fixing only the TVI table | The residual was two more real defects; a wider gate would have buried them | Tolerances are not a diagnosis |

## Consequences

- **Positive**: HIP CAMBI goes from `0.00000000` to **bit-exact** with the CPU
  (`5.8461540042` on both) on banding content. Metal gets the same two of the
  three fixes; it cannot be run here (no Apple hardware), so its result is
  asserted by construction rather than measured.
- **Negative**: any recorded HIP CAMBI score is invalid — it was zero or close
  to it on any content CAMBI is meant to detect. No in-tree snapshot covers a
  HIP CAMBI run.
- **Neutral / follow-ups**: the CUDA and SYCL CAMBI parity tests keep their own
  gradient fixture, which also scores `0.0`; CUDA has a second *textured*
  fixture that scores `0.173` and does assert something. Giving those two the
  10-bit banding fixture is a follow-up, tracked in the research digest.

## References

- Findings 71 / 72 / 73 / 74 from the twin-drift sweep: the HIP and Metal twins
  compute `tvi_for_diff` with a hand-rolled binary search over an inverted,
  non-monotone predicate instead of porting the CPU's
  `tvi_hard_threshold_condition` bisection, producing a completely different TVI
  table at default settings; and their `vlt_luma` is the largest luma below the
  threshold where the CPU takes the smallest luma at or above it.
- Measured on gfx1030 with a 640x480 10-bit banding gradient: before,
  `cpu = 5.84615400` / `hip = 0.00000000`; after all three fixes,
  `cpu = 5.8461540042` / `hip = 5.8461540042`. CUDA is bit-exact on the same
  fixture both before and after, which is what isolated the two HIP-only
  defects.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — the GPU parity CI gate.
