---
paths:
  - core/src/feature/metal/integer_cambi_metal.mm
  - core/src/feature/metal/integer_cambi.metal
invariant: CAMBI uses shared TVI helper and CPU's border rules (ADR-1219).
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# CAMBI: use the shared TVI helper and the CPU's border rules (ADR-1219)

Three exact-logic traps, all of which HIP twin fell into and which
together collapsed its CAMBI score to **exactly 0.0** on banding
content CPU scores at 5.85.

1. **Call `vmaf_cambi_init_tvi_and_vlt()`; never re-derive TVI
   table.** It runs CPU's own bisection of
   `tvi_hard_threshold_condition` between `luma_range.foot` and
   `luma_range.head - diff - 1`, plus `vlt_luma` and derived-band
   validation. Two independent hand-ports (HIP and Metal) both
   searched *negated* predicate seeded from luma 0, giving
   `tvi_for_diff = [1026, 1025, 1024, 4]` against CPU's
   `[182, 309, 436, 563]`. Host-side scalar work done once in
   `init()`, so per-backend copy buys nothing.

2. **`cambi.c::filter_mode` leaves output rows 0 and `height-1`
   UNFILTERED.** Its vertical writeback is under `if (i > 1)` and
   covers rows `1 .. height-2`; horizontal results for border rows
   live only in 3-row ring and are never written back. Kernel guard
   is `if (axis == 1 && (y == 0 || y >= height - 1)) return;` — V
   pass writes into buffer that still holds pre-filter image, so
   returning early preserves original pixels exactly.

3. **`get_spatial_mask_for_index()` ZERO-PADS its 7x7 box sum.**
   Summed-area table is `memset` to zero and gated by
   `deriv_valid = (i < height)`, so out-of-frame tap adds nothing.
   Clamping taps to border pixel counts its zero-derivative flag up
   to three extra times per axis and flips `box_sum > mask_index` on
   band of border pixels.

CAMBI parity fixture must band for real: CAMBI counts neighbour
differences of `1 .. num_diffs` (4 at default), so 8-bit gradient
stepping 32 levels every 32 columns scores 0.0 on CPU too and makes
assertion `0 == 0`. Use 10-bit gradient of one level every two
columns inside TVI band (200..900) and assert CPU score is
non-degenerate first.
