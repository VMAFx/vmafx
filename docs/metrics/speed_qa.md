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

## Non-finite scores fail the frame (speed_chroma / speed_temporal)

A SpEED score that is not finite is **not published**. The extractor logs a
warning naming the extractor, the feature, the frame index and the value, and
the frame fails with `-EINVAL`:

```text
libvmaf WARNING speed_chroma_cuda: non-finite speed_chroma_uv at frame 42 (score=nan), failing frame
```

This is a change from earlier releases. The score used to be bounded with a
less-than comparison against `speed_max_val`, and every comparison against NaN
is false, so a NaN was emitted as `speed_max_val` itself — a finite, plausible
1000.0 that no caller could distinguish from a real measurement. `-Inf` passed
the comparison and was published unclamped.

If you see this warning, the frame's SpEED score is missing rather than wrong.
The usual cause is a numerically degenerate block covariance — a flat or
linearly-graded chroma plane — on which the solved system is ill-conditioned.
Scores for other frames and other features are unaffected.

`speed_max_val` itself is unchanged: a finite score above it is still clipped
to it, on every backend. It now applies to `speed_temporal` on the CPU as well,
which previously declared the option and ignored it while the GPU backends
honoured it.

See [ADR-1301](../adr/1301-speed-nonfinite-score-fails-frame.md).

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
paths reproduce the CPU reference algorithm. The SYCL and CUDA twins run the
whole chain on the device and reproduce the CPU's fp32 arithmetic, so their
scores equal the CPU's to the last bit (see
[below](#sycl-device-resident-and-bit-identical-to-the-cpu) and
[the CPU's `log2f`](#the-cpu-reference-and-log2f) for the one condition); the
HIP twins agree within the fork's cross-backend tolerance (≤ 1e-4 relative),
checked by `core/test/test_{cuda,sycl,hip}_speed_{chroma,temporal}_parity`.

Two earlier algorithm defects in the GPU kernels are corrected:

- **Global covariance.** The mean/covariance kernels now compute a single
  covariance over the full phase-shifted 5×5 submatrix (a `means[25]` window),
  matching the CPU reference. The previous kernels computed per-tile,
  block-local statistics, which understated the score roughly seven-fold.
- **Separate reference/distorted bases.** The reference and distorted entropy
  terms now use independent covariance and eigenvalue bases. The previous
  kernels reused the reference basis for the distorted plane, biasing the
  chroma score high whenever the reference and distorted frames differed.

### Singular covariance matrices

SpEED's 25x25 covariance matrix counts as regular only when **every** eigenvalue
is at least `1e-6`. Anything flatter is singular, which happens routinely:
grayscale sources, solid-colour frames, letterbox and pillarbox bars, and — for
`speed_temporal` — any static passage, whose frame difference is identically
zero.

The CPU zeroes the solution on a singular plane and, when **exactly one** of the
reference and distorted sides is singular, returns `0` rather than the inflated
score a one-sided zero solution produces. `speed_chroma` additionally imputes
`speed_chroma_uv` from whichever of U/V survived.

Up to and including v3.2.1 the GPU twins diverged from this on two counts:

- The three `speed_temporal` twins never reported singularity to their caller,
  so the one-sided rule did not exist there and they returned the score kernel's
  result. On a 960x960 fixture with a frozen reference and a moving distorted
  side the CPU returns `0.00000000` and every GPU backend returned
  `230.71379089`. **Re-measure any GPU `speed_temporal` score taken over content
  with static passages.** (`speed_chroma` was fixed earlier, in ADR-1202.)
- All six twins zeroed a *host* staging buffer on the singular path and uploaded
  nothing, so the score kernel read the device solution left over from the
  previous frame — or, on the first frame, whatever the allocator returned. This
  one has no demonstrable effect on the emitted score, because when both sides
  are singular the CPU's own zeroed solution drives every block's variance to
  `0` and the score to exactly `0` regardless; it is fixed because reading
  uninitialised device memory is undefined behaviour.

Both are corrected per
[ADR-1218](../adr/1218-gpu-speed-singular-device-solution.md) and gated by
`core/test/test_{cuda,sycl,hip}_speed_singular_parity`.

No usage change is required — backend selection is automatic. To force a
specific backend for a cross-backend parity check, use the fork's `--backend`
selector. See [backends/cuda/overview](../backends/cuda/overview.md) and
[backends/sycl/overview](../backends/sycl/overview.md).

### SYCL: device-resident and bit-identical to the CPU

Since [ADR-1358](../adr/1358-sycl-speed-device-resident-linalg.md),
`speed_chroma_sycl` and `speed_temporal_sycl` run the whole per-frame chain on
the device: picture conversion (and, for `speed_temporal`, the frame
difference), the optional prescale, the anti-alias filter at the decimated
sample points, local mean subtraction, the 25x25 covariance, its eigenvalues,
the regularity decision, the QR solve and the score. Each frame costs one upload
of the raw planes and one read of the result; the host no longer filters,
factorises or waits in between. The chain is recorded once as a SYCL graph and
replayed per frame where the device supports graphs.

Every stage reproduces the CPU extractor's arithmetic in fp32, so the SYCL
scores are not merely within tolerance: on the Netflix 576x324 pair (48 frames)
and 50 frames of BBB 3840x2160, every per-frame `speed_chroma_u`,
`speed_chroma_v`, `speed_chroma_uv` and `speed_temporal` value is identical to
`--backend cpu` at `--precision max`, on an Arc B580 and on a UHD 770. Before
this change 1 to 9 frames per run matched and the largest difference was
4.2e-5. Two option paths are not exact. Builds with AdaptiveCpp lack the
correctly rounded division fallback and keep the ADR-0214 tolerance.
`speed_prescale_method=lanczos4` evaluates its kernel weights in fp32 where the
CPU uses fp64 `sin`, and SpEED amplifies the few-ulp weight differences: the
CUDA twin, which computes the weights the same way, is up to 2.1e-2 (5.7e-4
relative) from the CPU on a smooth 1920x1080 gradient with
`speed_prescale=0.5` on an RTX 4090, beyond the ADR-0214 tolerance, and at
most 2.5e-5 on natural content
(`T-GPU-SPEED-LANCZOS4-PRESCALE-DRIFT-2026-09-30` in [`state.md`](../state.md)).

Milliseconds per frame, `(t(22) - t(2)) / 20`, median of 3, one Arc B580 and
one UHD 770 through WSL2 Level Zero, i9-12900K, the icx/icpx 2026.1 build of the
`vmaf-dev-mcp` image. The CPU row is `--backend cpu --threads 16`, which runs
frames in parallel; the GPU rows run one frame at a time. At 576x324 the
startup-free difference is within run-to-run noise, so those rows use 48 frames
and 5 repetitions:

| Feature | Size | CPU (16 threads) | B580 before | B580 after | UHD 770 before | UHD 770 after |
|---|---|---:|---:|---:|---:|---:|
| `speed_chroma` | 576x324 | 0.16 | 3.60 | 0.89 | 17.31 | 3.44 |
| `speed_chroma` | 3840x2160 | 7.24 | 23.31 | 7.51 | 37.98 | 14.48 |
| `speed_temporal` | 576x324 | 0.85 | 2.19 | 0.83 | 8.32 | 3.47 |
| `speed_temporal` | 3840x2160 | 37.27 | 60.37 | 7.58 | 72.15 | 18.27 |

At 4K both twins now run at about the rate the CLI reads 4K frames from disk;
`speed_temporal`, which the CPU cannot run frames in parallel for, is about
five times faster than the CPU. At 576x324 the eigenvalue sweep, which runs on
one work item for the reference's operation order, keeps the twins at about a
millisecond per frame on the B580.

The rows above request the twin by its registered name. Since
[ADR-1359](../adr/1359-cli-feature-backend-twin.md), `--backend sycl --feature
speed_chroma` runs the twin as well and the JSON `feature_backends` array names
it; before that, the CPU name ran the CPU extractor on one thread (18.35 ms per
frame at 4K on the machine above):

```sh
vmaf -r ref.yuv -d dis.yuv -w 3840 -h 2160 -p 420 -b 8 --no_prediction \
  --backend sycl --feature speed_chroma_sycl -o out.json --json
```

### CUDA: the same chain on the device

Since [ADR-1380](../adr/1380-cuda-speed-device-resident-pipeline.md),
`speed_chroma_cuda` and `speed_temporal_cuda` run the SYCL chain above on CUDA,
in one pipeline both extractors share. They take their planes from the picture
the CUDA engine already uploaded, with device-to-device copies (`speed_temporal`
keeps the previous frame's luma on the device), and read back one 40-byte
result per frame; `collect()` is the only wait. Every rounding the CPU performs
is spelled with a round-to-nearest intrinsic (`__fadd_rn`, `__fmul_rn`,
`__fdiv_rn`, `__fsqrt_rn`, ...), which nvcc never fuses, and the kernels are
also built with `--fmad=false`.

```sh
vmaf -r ref.yuv -d dis.yuv -w 3840 -h 2160 -p 420 -b 8 --no_prediction   --backend cuda --feature speed_chroma_cuda -o out.json --json
```

On an RTX 4090, against an icx build of the CPU extractor at `--precision
max`, every per-frame `speed_chroma_u`, `speed_chroma_v`, `speed_chroma_uv`
and `speed_temporal` value is identical to `--backend cpu` on the Netflix
576x324 pair (48 frames) and BBB 3840x2160 (50 frames), and so are nearest,
bilinear and bicubic prescale; `lanczos4` is not exact
(`T-GPU-SPEED-LANCZOS4-PRESCALE-DRIFT-2026-09-30`). Before ADR-1380, 1 to 10
frames per output matched, and `speed_temporal_cuda` failed at 1920x1080 and
above with `CUDA_ERROR_INVALID_VALUE`. Milliseconds per frame,
`(t(N) - t(2)) / (N - 2)`, median of 3, before and after in alternation on the
same host ([Research-1379](../research/1379-cuda-cambi-speed-device-resident.md)
has the method and the raw numbers):

| Feature | Size | CUDA before | CUDA after | CPU, 16 threads |
|---|---|---:|---:|---:|
| `speed_chroma` | 3840x2160 | 24.90 | 6.89 | 9.11 |
| `speed_chroma` | 576x324 | 1.51 | 0.44 | 0.12 |
| `speed_temporal` | 3840x2160 | fails | 5.88 | 26.74 |
| `speed_temporal` | 576x324 | 2.81 | 0.42 | 0.92 |

The absolute numbers move with the host's load; what holds across runs is
that at 4K the twins now cost about what the trivial `psnr_cuda` costs. In a
run at load average 13, with N = 102, `psnr_cuda` took 2.41 to 2.56 ms per
frame, `speed_chroma_cuda` 2.75 and `speed_temporal_cuda` 2.73: reading and
uploading the pictures, not the SpEED kernels, sets their speed.

The HIP twins still read the covariance back and run the linear algebra on the
host; porting the chain to them is tracked as
`T-HIP-SPEED-HOST-RESIDUAL-2026-09-29`.

### The CPU reference and `log2f`

The CPU extractor calls `log2f` from the platform's C library about 25 times
per block, and the device twins compute a correctly rounded `log2` instead.
glibc's `log2f` rounds 0.14 % of the floats in [1, 1024) the other way (glibc
2.43), Intel's libimf 0.00013 %. So "identical to the CPU" holds against a CPU
build whose `log2f` rounds the arguments it meets correctly, such as a
`CC=icx CXX=icpx meson setup` build. Against a gcc or clang build on glibc, a
few frames differ in the last bits, well inside the ADR-0214 tolerance: with
glibc 2.43, 6 of 48 `speed_chroma_u` frames on the Netflix pair, by at most
4.8e-7; with glibc 2.44, zero to two frames per `speed_chroma` output on the
Netflix pair and on BBB 4K, by at most 1.4e-6. The CPU extractor itself then
differs by the same amount between its gcc and icx builds.

The CPU build must not fuse multiply-adds either. icx does when FMA
instructions are available, for example with `-march=native`, which is how the
`vmaf-dev-mcp` image builds its own `/usr/local/bin/vmaf`: that binary's
SpEED scores are up to 7.9e-4 from a default build at 1080p. Compare the GPU
twins against a build without `-march=native`
([Research-1379](../research/1379-cuda-cambi-speed-device-resident.md)).

### Checking a GPU twin against the CPU

`scripts/dev/speed_gpu_parity.py` runs the CPU extractor and a GPU twin over the
two fixtures above, prints the bit-identical frame count and the largest
difference per output, then times both the same way as the table. It exits 0
only when every output of every frame is identical:

```sh
python3 scripts/dev/speed_gpu_parity.py --backend cuda \
  --vmaf build/tools/vmaf --netflix-dir python/test/resource/yuv --bbb-dir testdata/bbb
# SYCL: pick the device first, e.g. ONEAPI_DEVICE_SELECTOR=level_zero:0
```

`--no-timing` skips the timing runs; `--reps` and `--threads` change the
repetitions and the CPU thread count. The CPU side runs from the same `vmaf`
binary, so build it with icx (as the dev image does) for an exact comparison;
with a gcc build expect the few [`log2f`](#the-cpu-reference-and-log2f) frames
to differ and the script to exit 1.

## "Covariance matrix singular" in the log

On content whose chroma is flat or a smooth gradient — a desaturated scene, a
solid background, animation with large constant areas — the 25x25 covariance
matrix SpEED solves per frame has no rank-25 estimate, and the run prints:

```text
libvmaf WARNING speed_chroma_cuda: covariance matrix singular, zeroing solution
  — further occurrences are counted and reported once at close
libvmaf WARNING speed_chroma_cuda: covariance matrix was singular on 192 of 192 solves
```

**This is not an error and not a backend defect.** Every implementation, CPU and
GPU alike, handles it the same way: the solution is zeroed for that solve and
scoring continues. Where exactly one of the two chroma channels is singular, the
`uv` score is imputed from the other channel, matching the CPU reference.

What the two lines tell you is how much of the run was affected. `192 of 192`
means every solve was singular, so the SpEED chroma scores for that clip carry
no information and should not be read as quality differences; a small count on a
long run is ordinary and can be ignored. The notice is emitted once when it
first happens and once at close, rather than once per solve — a per-solve notice
is four lines a frame per channel and buries the rest of the output.

If you need the scores on such content, there is nothing to configure: the
covariance is singular because the content has no chroma detail at the
5x5 block scale SpEED works on, not because of a setting.

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

Run the existing CPU tests in a build configured with `-Denable_float=true`
(the chroma/temporal registration test is float-gated):

```sh
python3 "$(git rev-parse --show-toplevel)/scripts/ci/run_meson_test.py" -- \
  -C build --print-errorlogs test_speed test_speed_qa
```

The two executables retain five registered cases each. The temporal QA test
uses small setup helpers that propagate the original assertion failure to
the same test runner; its 64×64 inputs and 0-to-255 frame step are unchanged.
