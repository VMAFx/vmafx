<!-- markdownlint-disable MD060 -->
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

```text
mu      = sum_ij( w(i,j) * p(i,j) ) / sum_ij( w(i,j) )
sigma^2 = sum_ij( w(i,j) * p(i,j)^2 ) / sum_ij( w(i,j) ) - mu^2
```

Pixel values are in [0, 255] for 8-bpc input. For HBD (10 or 12 bpc) input
the pixels are normalised to the 8-bpc range before weighting.

### Per-block entropy

```text
H(block) = 0.5 * log2( 2 * pi * e * (sigma^2 + epsilon) )
```

where `epsilon = 1.0 pixel^2` is a noise floor that prevents log(0) on
perfectly flat (constant-valued) blocks.

### Spatial score

The spatial score S for frame n is the mean per-block entropy over the
distorted luma plane:

```text
S(n) = mean_i( H_i )
```

### Temporal score

The temporal score T is computed identically to S but on the frame-difference
image:

```text
delta(i,j) = dist(n, i, j) - dist(n-1, i, j)
T(n)       = mean_i( H_i(delta) )    for n > 0
T(0)       = 0
```

The extractor stores the previous distorted frame internally.

### Combined output

```text
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

## Relationship to speed_chroma and speed_temporal

The fork also carries the upstream Netflix full-reference SpEED extractors:

- `speed_chroma` -- FR SpEED score on the U/V chroma channels.
  Requires `-Denable_float=true`.
- `speed_temporal` -- FR SpEED score on luma frame-differences.
  Requires `-Denable_float=true`.

Both use the full GSM prior model with eigenvalue decomposition of block
covariance matrices (more accurate but more expensive than `speed_qa`'s
simpler local-variance estimator). `speed_qa` is a lightweight alternative
that does not require float compilation.

## CPU SIMD dispatch (speed_chroma / speed_temporal)

Two parts of the CPU SpEED path pick a vector kernel at runtime from the
instruction sets the host actually reports:

| Kernel | Scalar | AVX2 | AVX-512 |
| --- | --- | --- | --- |
| Block covariance sum | yes | yes | yes |
| Dense matrix product (QR factorisation and the `QᵀB` solve) | yes | yes | yes |

Nothing has to be enabled: the widest supported kernel is chosen when the
extractor initialises. `--cpumask` restricts the choice, because its bits name
the instruction sets to *disable* — `--cpumask 16` forbids AVX-512 and falls
back to AVX2, `--cpumask 24` forbids AVX2 as well and falls back to scalar.

**The scores do not depend on which kernel runs.** The vectorised axis of the
matrix product is an output index rather than an accumulation axis, so widening
it cannot reorder any element's arithmetic, and the two translation units are
compiled with floating-point contraction disabled so no multiply/add pair
collapses into a differently-rounded FMA. `vmaf --precision=max` output is
byte-identical across all three settings; `core/test/test_speed_simd`
enforces that with exact binary comparison against the scalar reference. Use
`--cpumask` when you want to compare timings, not to chase a score difference
— there is not one to find.

Picking the widest kernel is worth roughly 1.2x on the whole default-model
run (`vmaf_v1.0.16_3d0h`) on an AVX-512 host; see
[ADR-1196](../adr/1196-speed-matmul-simd-dispatch.md) and
[research digest 2030](../research/2030-speed-matmul-and-cambi-cpu-hot-path.md)
for the profile and the measurement method. Models that do not carry a SpEED
feature — `vmaf_v0.6.1.json`, for instance — never reach these kernels and are
unaffected either way.

## GPU backend parity (speed_chroma / speed_temporal)

`speed_chroma` and `speed_temporal` carry CUDA, HIP, and SYCL implementations
that are selected at runtime when the corresponding backend is active. The GPU
paths reproduce the CPU reference algorithm. As of the SpEED GPU correctness
fix they agree with the CPU score to within the fork's cross-backend tolerance
(≤ 1e-4 relative); the CUDA path is additionally bit-parity-verified against
the CPU reference on an RTX 4090 via `core/test/test_cuda_speed_chroma_parity`
and `core/test/test_cuda_speed_temporal_parity`.

Two earlier algorithm defects in the GPU kernels are corrected:

- **Global covariance.** The mean/covariance kernels now compute a single
  covariance over the full phase-shifted 5×5 submatrix (a `means[25]` window),
  matching the CPU reference. The previous kernels computed per-tile,
  block-local statistics, which understated the score roughly seven-fold.
- **Separate reference/distorted bases.** The reference and distorted entropy
  terms now use independent covariance and eigenvalue bases. The previous
  kernels reused the reference basis for the distorted plane, biasing the
  chroma score high whenever the reference and distorted frames differed.

No usage change is required — backend selection is automatic. To force a
specific backend for a cross-backend parity check, use the fork's `--backend`
selector. See [backends/cuda/overview](../backends/cuda/overview.md) and
[backends/sycl/overview](../backends/sycl/overview.md).

## Python compat wrappers

The compat Python harness (`compat/python-vmaf/`) ships Python wrappers for
both full-reference SpEED extractors, ported from Netflix upstream per the
Research-0732 audit (PR #22):

| Class | Module | Feature flag |
| --- | --- | --- |
| `SpeedChromaFeatureExtractor` | `vmaf.core.feature_extractor` | `speed_chroma` |
| `SpeedTemporalFeatureExtractor` | `vmaf.core.feature_extractor` | `speed_temporal` |
| `SpeedChromaQualityRunner` | `vmaf.core.quality_runner` | via `speed_chroma_uv` |
| `SpeedChromaUQualityRunner` | `vmaf.core.quality_runner` | via `speed_chroma_u` |
| `SpeedChromaVQualityRunner` | `vmaf.core.quality_runner` | via `speed_chroma_v` |
| `SpeedTemporalQualityRunner` | `vmaf.core.quality_runner` | via `speed_temporal` |

Usage:

```python
from vmaf.core.feature_extractor import SpeedChromaFeatureExtractor
from vmaf.core.asset import Asset

asset = Asset(dataset="test", content_id=0, asset_id=0,
              ref_path="ref.yuv", dis_path="dis.yuv",
              asset_dict={"width": 1920, "height": 1080})

fextractor = SpeedChromaFeatureExtractor([asset], None)
fextractor.run()
result = fextractor.results[0]
print(result["Speed_chroma_feature_speed_chroma_uv_score"])
```

These wrappers call the `vmafexec` binary with `--feature speed_chroma` or
`--feature speed_temporal` respectively and parse the resulting XML log.
The C extractors must have been compiled with `-Denable_float=true`.

## Implementation notes

- **No float dependency.** `speed_qa.c` is compiled unconditionally.
  It does not depend on `speed.c` (float-gated).
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
