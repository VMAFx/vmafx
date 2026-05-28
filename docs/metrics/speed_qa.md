# SpEED-QA Feature Extractor

**Feature name:** `speed_qa`
**ADR:** [ADR-0253](../adr/0253-speed-qa-extractor.md)
**Reference:** Bampis, Gupta, Soundararajan and Bovik, "SpEED-QA: Spatial
Efficient Entropic Differencing for Image and Video Quality",
*IEEE Signal Processing Letters* 24(9), 1333-1337, 2017.
DOI [10.1109/LSP.2017.2726542](https://doi.org/10.1109/LSP.2017.2726542)

## Overview

`speed_qa` is a per-frame quality feature derived from the local spatial
entropy of the distorted luma plane and the entropy of the inter-frame pixel
difference. It operates on the distorted signal only for the spatial component,
augmented by a temporal component that captures motion-induced change.

The output is a scalar score per frame. Higher values indicate higher local
entropy (more texture or inter-frame change). The feature is designed to be
used as an input to a downstream quality model rather than as a standalone
quality predictor.

## Algorithm

### Block partitioning

The distorted luma plane is divided into non-overlapping **7x7 pixel blocks**.
Only complete blocks are used; the right and bottom margins (at most 6 pixels)
are discarded. A 720p frame (1280x720) yields 182 x 102 = 18,564 blocks.

### Gaussian-windowed local variance

Within each block, a **separable 7-tap Gaussian kernel** (sigma = 1.166,
matching the VIF family) computes the weighted local mean and variance:

```
mu      = sum_ij( w(i,j) * p(i,j) ) / sum_ij( w(i,j) )
sigma^2 = sum_ij( w(i,j) * p(i,j)^2 ) / sum_ij( w(i,j) ) - mu^2
```

Pixel values are in [0, 255] for 8-bpc input. For HBD (10 or 12 bpc) input
the pixels are normalised to the 8-bpc range before weighting.

### Per-block entropy

```
H(block) = 0.5 * log2( 2 * pi * e * (sigma^2 + epsilon) )
```

where `epsilon = 1.0 pixel^2` is a noise floor that prevents log(0) on
perfectly flat (constant-valued) blocks.

### Spatial score

The spatial score S for frame n is the mean per-block entropy over the
distorted luma plane:

```
S(n) = mean_i( H_i )
```

### Temporal score

The temporal score T is computed identically to S but on the frame-difference
image:

```
delta(i,j) = dist(n, i, j) - dist(n-1, i, j)
T(n)       = mean_i( H_i(delta) )    for n > 0
T(0)       = 0
```

The extractor stores the previous distorted frame internally.

### Combined output

```
score(n) = S(n) + T(n)
```

## Usage

```bash
vmaf --reference ref.yuv --distorted dist.yuv \
     --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
     --feature speed_qa --output output.xml
```

No build flags are required: `speed_qa` is compiled unconditionally
(no `-Denable_float=true` needed).

## Implementation notes

- **No float dependency.** `speed_qa.c` is compiled unconditionally.
- **Integer pixel reads, double accumulation.** Luma is read directly as
  `uint8_t` (8-bpc) or `uint16_t` (HBD) without intermediate float buffers.
- **Gaussian weights are Q16 fixed-point** (kernel sum = 65535). The 2-D
  weight for pixel (i,j) is `g[i] * g[j] / 65535^2`.
- **VMAF_FEATURE_EXTRACTOR_TEMPORAL** flag ensures in-order frame delivery.
  The extractor maintains its own `prev_dist` buffer (aligned, private).

## Test coverage

`core/test/test_speed_qa.c` provides five smoke tests:

1. Registration by name and feature-name round-trip.
2. VTable completeness (init/extract/close non-NULL, priv_size > 0).
3. Flat grey input produces a finite, non-NaN score.
4. Noise-textured (checkerboard) input produces a higher score than flat.
5. A 0-to-255 inter-frame step raises frame-1 score above frame-0 score
   (confirming the temporal component is positive).
