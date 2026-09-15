<!-- markdownlint-disable MD013 -->

# 2036 — Three CAMBI divergences behind one vacuous fixture

**Date**: 2026-09-07
**Scope**: `integer_cambi` on the HIP and Metal backends.
**Outcome**: three defects fixed; HIP CAMBI goes from `0.00000000` to bit-exact
with the CPU ([ADR-1219](../adr/1219-gpu-cambi-tvi-shared-bisection.md)).

## The headline

HIP CAMBI returned **exactly zero** on content the CPU scores at `5.85`. Not a
tolerance drift — a total collapse, on default options, on any banding input.
It shipped because the parity test asserted `0 == 0`.

## Defect 1 — a hand-ported bisection, inverted

`cambi.c::get_tvi_for_diff()` bisects `tvi_hard_threshold_condition` between
`luma_range.foot` (64) and `luma_range.head - diff - 1`, returning the largest
sample at which `delta_luminance > tvi_threshold * mean_luminance` still holds.

The HIP and Metal twins each wrote their own search over the **negated**
predicate (`diff_lum < threshold * sample_lum`), seeded from luma `0` rather
than `foot`. Replicating both against the same luminance model at the default
`max_log_contrast = 2`:

| `diff` | CPU bisection | hand-rolled |
| --- | --- | --- |
| 1 | 182 | 1026 |
| 2 | 309 | 1025 |
| 3 | 436 | 1024 |
| 4 | 563 | 4 |

`v_band_size = tvi_for_diff[num_diffs-1] + 1 - v_band_base`, so the scored luma
band collapses from 564 entries to a handful, and `calculate_c_values()`
discards almost every pixel as out-of-band. The same twins also derived
`vlt_luma` as the *largest* luma below the visibility threshold, where the CPU
takes the *smallest* luma at or above it — an exact off-by-one, invisible only
because the default threshold makes both return `0`.

The SYCL twin calls the shared `vmaf_cambi_init_tvi_and_vlt()` and has never
had any of this. That is the fix for HIP and Metal too: the table is host-side
scalar work done once in `init()`, so a per-backend copy buys nothing and costs
exactly this.

## Defect 2 — `filter_mode`'s border rows

```c
for (int i = 0; i < height; i++) {
    /* horizontal pass into a 3-row ring `buffer` */
    if (i > 1) {
        for (int j = 0; j < width; j++)
            data[(i - 1) * stride + j] = mode3(buffer[0*w+j], buffer[1*w+j], buffer[2*w+j]);
    }
}
```

The writeback covers output rows `1 .. height-2`. Rows `0` and `height-1` keep
the **original, unfiltered** pixels — not the horizontally-filtered ones, which
never leave the ring. A twin that runs a clean separable H-then-V pass over all
rows is wrong at both edges. CUDA and SYCL already carried the guard; HIP and
Metal did not.

## Defect 3 — the mask box sum clamps instead of zero-padding

`get_spatial_mask_for_index()` accumulates a zero-padded summed-area table:
`memset(dp, 0, ...)`, `dp_width = width + 2*pad_size + 1`, and
`deriv_valid = (i < height)`. An out-of-frame tap contributes nothing.

The HIP kernel clamped each of the 49 taps to the border pixel, counting that
pixel's zero-derivative flag up to three extra times per axis and flipping
`box_sum > mask_index` on a band of border pixels. CUDA (via a shared-memory
tile that writes `0` outside the frame) and Metal (explicitly, with a comment
saying clamping "is INCORRECT") both already zero-pad.

## Why nothing caught it: the fixture did not band

Both the HIP and Metal CAMBI parity fixtures were 8-bit gradients stepping **32
code levels every 32 columns**:

```c
const unsigned base = (col / 32u) * 32u + salt * 4u;
```

CAMBI counts neighbour differences of `1 .. num_diffs` — 4 at the default
`max_log_contrast = 2`. A 32-level step is an edge, not banding. Measured: that
fixture scores `0.0` on the **CPU** as well, so the parity assertion compared
zero to zero and passed against a twin whose score had collapsed to zero for an
entirely different reason.

The replacement is a 10-bit gradient of one code level every two columns held in
`200..900` so the whole plane sits inside the TVI band. It scores `5.846154` on
the CPU. Both tests now also assert `cpu > 1.0` before comparing, so the gate
cannot rot back to `0 == 0`.

## How the three were separated

Fixing only defect 1 left a `6.15e-3` residual against a `1e-3` gate — enough to
look like "GPU float noise" and be waved through with a wider tolerance. Running
the **CUDA** twin on the identical fixture gave `cpu = gpu = 5.8461540042`,
bit-exact, which proved the residual was HIP-specific rather than a general
GPU/CPU property. Diffing the HIP kernels against the CUDA ones then located
defects 2 and 3 directly.

The general move: when a twin is close but not exact, find a *second* twin that
is exact on the same input before touching the tolerance. A wider gate would
have buried two real defects here.

## Follow-up

The CUDA and SYCL CAMBI parity tests keep their own gradient fixture, which
scores `0.0` for the same reason. CUDA has a second *textured* fixture scoring
`0.173`, so it does assert something; SYCL should be checked. Giving both the
10-bit banding fixture is a mechanical follow-up.
