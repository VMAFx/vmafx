<!-- markdownlint-disable MD060 -->
# SpEED Full-Reference Extractors (speed_chroma, speed_temporal)

`speed_chroma` and `speed_temporal` are the full-reference SpEED (Spatial
Efficient Entropic Differencing) extractors the fork carries from Netflix/vmaf.
They return the values Netflix/vmaf returns for the same frames, and the
`vmaf_v1.0.16` models read them as features.

- `speed_chroma` scores the U and V chroma channels.
- `speed_temporal` scores the luma frame differences.

Both use the full Gaussian-scale-mixture (GSM) prior model with eigenvalue
decomposition of block covariance matrices. That is more accurate and more
expensive than the lightweight local-variance estimator of the no-reference
[`speed_qa`](speed_qa.md) feature. They are compiled when the `enable_float`
Meson option is on, which is the default.

## How to run

```bash
vmaf -r ref.yuv -d dis.yuv -w 1920 -h 1080 -p 420 -b 8 --no_prediction \
     --feature speed_chroma --feature speed_temporal --json -o out.json
```

Options follow the feature name after the first `=`, separated by `:`:

```bash
vmaf -r ref.yuv -d dis.yuv -w 1920 -h 1080 -p 420 -b 8 --no_prediction \
     --feature speed_chroma=speed_prescale=0.5:speed_prescale_method=bicubic \
     --json -o out.json
```

A GPU backend runs the twin automatically when `--backend cuda|sycl|hip` is
given ([ADR-1359](../adr/1359-cli-feature-backend-twin.md)); the JSON
`feature_backends` array names the extractor that ran.

## Output features

| Extractor | Features | Notes |
| --- | --- | --- |
| `speed_chroma` | `speed_chroma_u`, `speed_chroma_v`, `speed_chroma_uv` | Input must have chroma planes; 4:0:0 is refused. |
| `speed_temporal` | `speed_temporal` | Scores the difference between consecutive luma frames. |

When an option has a non-default value, the output names carry it as a suffix,
for example `speed_chroma_u_ps_0.5_psm_bicubic` for the command above (the
suffix uses the option's alias and its value).

A finite score above `speed_max_val` is clipped to it; a score that is not
finite fails the frame (see
[Non-finite scores](#non-finite-scores-fail-the-frame)).

## Options

| Option | Alias | Extractor | Default | Range | Effect |
| --- | --- | --- | --- | --- | --- |
| `speed_kernelscale` | `ks` | both | `1.0` | 0.1 to 4.0 | Scaling factor for the Gaussian kernel (2.0 doubles the standard deviation and enlarges the kernel accordingly). |
| `speed_prescale` | `ps` | both | `1.0` | 0.1 to 4.0 | Scaling factor for the frame (2.0 makes the image twice as large on each side). |
| `speed_prescale_method` | `psm` | both | `nearest` | `nearest`, `bilinear`, `bicubic`, `lanczos4` | Scaling method for the frame. |
| `speed_sigma_nn` | `snn` | both | `0.29` | 0.1 to 2.0 | Standard deviation of neural noise. |
| `speed_nn_floor` | `nnf` | both | `0.0` | 0.0 to 1.0 | Neural noise floor, as a fraction of `sigma_nn`. |
| `speed_max_val` | `mxv` | both | `1000.0` | 0.0 to 1000.0 | Maximum value allowed; larger finite scores are clipped to it, on every backend. |
| `speed_weight_var_mode` | `wvm` | `speed_chroma` | `0` | 0 to 6 | Approach to variance-based weighting. |
| `speed_use_ref_diff` | `urd` | `speed_temporal` | `false` | bool | Debug mode: enable additional output. |

The tables are from the `VmafOption` arrays of `core/src/feature/speed.c`.

## Agreement with Netflix's scores {#the-scores-are-netflixs}

`speed_chroma` and `speed_temporal` return the values Netflix/vmaf returns for
the same frames. Read through the C API at `%.17g` against a build of
Netflix/vmaf `cea2b4d8` (its SpEED sources are those of master, `9e48141b`):

- **Clips.** 21 clips for `speed_chroma` and 24 for `speed_temporal`, from
  160x90 to 3840x2160, 8 to 16 bits, 4:2:0, 4:2:2 and 4:4:4.
- **Values.** Every `speed_chroma_u`, `_v` and `_uv` value (261 frames each) and
  every `speed_temporal` value (320 frames) is identical, with the scalar
  kernels, the default dispatch and AVX2.
- **Models.** The scores of the four `vmaf_v1.0.16` models are identical too
  (204 frames each, scalar and default dispatch).
- **Compilers.** GCC and clang builds for x86-64 and aarch64 return the same
  bits, and so does an icx build.

The fork differs from Netflix where it decided to:

| Option or input | Netflix | This fork |
| --- | --- | --- |
| `speed_max_val` on `speed_temporal` | ignores the option | clamps, as on `speed_chroma` ([ADR-1301](../adr/1301-speed-nonfinite-score-fails-frame.md)) |
| `speed_prescale` above 1 on `speed_temporal` | reads past its frame buffers | sizes them for the scaled frame ([ADR-1480](../adr/1480-speed-frame-buffers-prescale-above-one.md)) |
| a plane too small for SpEED (below 80x80) | crashes | refuses the frame (`-EINVAL`, [ADR-1481](../adr/1481-extractor-failure-fails-the-run.md)) |
| a 4:0:0 input to `speed_chroma` | no output | refuses the frame ([ADR-1481](../adr/1481-extractor-failure-fails-the-run.md)) |

### Checking a build against Netflix's values

Run `test_speed_upstream_form` in a build configured with
`-Denable_float=true`:

```bash
meson test -C build test_speed_upstream_form
```

- **Every C library.** It holds `speed.c`'s rotation, entropy and score
  statements to Netflix's, evaluated in the test with the same C library's
  `sqrt()` and `log2()`.
- **glibc only.** It also compares 11 frames of `testdata/ref_576x324_48f.yuv`
  in four option sets with the values a Netflix build returns. Those values
  come from glibc's `log2()`, so with another C library (Windows, macOS, musl)
  the test prints that comparison as skipped and gives the reason.

## Non-finite scores fail the frame

A SpEED score that is not finite is **not published**. The extractor logs a
warning naming the extractor, the feature, the frame index and the value, and
the frame fails with `-EINVAL`
([ADR-1301](../adr/1301-speed-nonfinite-score-fails-frame.md)):

```text
libvmaf WARNING speed_chroma_cuda: non-finite speed_chroma_uv at frame 42 (score=nan), failing frame
```

If you see this warning, the frame's SpEED score is missing rather than wrong.
The usual cause is a numerically degenerate block covariance, such as a flat or
linearly-graded chroma plane, on which the solved system is ill-conditioned.
Scores for other frames and other features are unaffected.

`speed_max_val` is unchanged: a finite score above it is still clipped to it,
on every backend. It applies to `speed_temporal` on the CPU as well, which
previously declared the option and ignored it while the GPU backends honoured
it.

## Singular covariance matrices

SpEED's 25x25 covariance matrix counts as regular only when **every**
eigenvalue is at least `1e-6`. Anything flatter is singular, which happens
routinely: grayscale sources, solid-colour frames, letterbox and pillarbox
bars, and, for `speed_temporal`, any static passage, whose frame difference is
identically zero.

The rules are the same on the CPU and on every GPU twin:

- The solution is zeroed on a singular plane and scoring continues.
- When **exactly one** of the reference and distorted sides is singular, the
  score is `0` rather than the inflated score a one-sided zero solution
  produces.
- `speed_chroma` additionally imputes `speed_chroma_uv` from whichever of U/V
  survived.

### "Covariance matrix singular" in the log

On content whose chroma is flat or a smooth gradient (a desaturated scene, a
solid background, animation with large constant areas) the run prints:

```text
libvmaf WARNING speed_chroma_cuda: covariance matrix singular, zeroing solution
  — further occurrences are counted and reported once at close
libvmaf WARNING speed_chroma_cuda: covariance matrix was singular on 192 of 192 solves
```

!!! note "This is not an error and not a backend defect"
    Every implementation handles it the same way (see the rules above). The
    notice is emitted once when it first happens and once at close, rather
    than once per solve, because a per-solve notice is four lines a frame per
    channel and buries the rest of the output.

What the two lines tell you is how much of the run was affected:

- `192 of 192` means every solve was singular, so the SpEED chroma scores for
  that clip carry no information and should not be read as quality
  differences.
- A small count on a long run is ordinary and can be ignored.

There is nothing to configure: the covariance is singular because the content
has no chroma detail at the 5x5 block scale SpEED works on, not because of a
setting. The tracked fixture `testdata/ref_576x324_48f.yuv` against
`testdata/dis_576x324_48f.yuv` prints these lines on the CPU as well.

## CPU SIMD dispatch

Two parts of the CPU SpEED path pick a vector kernel at runtime from the
instruction sets the host reports:

| Kernel | Scalar | AVX2 | AVX-512 |
| --- | --- | --- | --- |
| Block covariance sum | yes | yes | yes |
| Dense matrix product (QR factorisation and the `QᵀB` solve) | yes | yes | yes |

Nothing has to be enabled: the widest supported kernel is chosen when the
extractor initialises. `--cpumask` restricts the choice, because its bits name
the instruction sets to *disable*:

- `--cpumask 16` forbids AVX-512 and falls back to AVX2.
- `--cpumask 24` forbids AVX2 as well and falls back to scalar.

**The scores do not depend on which kernel runs.** The vectorised axis of the
matrix product is an output index rather than an accumulation axis, so widening
it cannot reorder any element's arithmetic, and the two translation units are
compiled with floating-point contraction disabled so no multiply/add pair
collapses into a differently-rounded FMA. `vmaf --precision=max` output is
byte-identical across all three settings; `core/test/test_speed_simd` enforces
that with exact binary comparison against the scalar reference. Use `--cpumask`
to compare timings, not to chase a score difference.

Picking the widest kernel is worth roughly 1.2x on the whole default-model run
(`vmaf_v1.0.16_3d0h`) on an AVX-512 host; see
[ADR-1196](../adr/1196-speed-matmul-simd-dispatch.md) and the
[research digest](../research/2030-speed-matmul-and-cambi-cpu-hot-path.md) for
the profile and the measurement method. Models that do not carry a SpEED
feature, such as `vmaf_v0.6.1.json`, never reach these kernels.

## GPU twins

`speed_chroma` and `speed_temporal` carry CUDA, HIP and SYCL implementations
that are selected at runtime when the corresponding backend is active. All
three return the CPU extractor's scores bit for bit on every C library, checked
with `==` by `core/test/test_{cuda,sycl,hip}_speed_{chroma,temporal}_parity`.

| Backend | Twins | ADR | Exact vs CPU | Evidence (fragments in `scripts/ci/exact_twins.d/`) |
| --- | --- | --- | --- | --- |
| CUDA | `speed_chroma_cuda`, `speed_temporal_cuda` | [ADR-1380](../adr/1380-cuda-speed-device-resident-pipeline.md), [ADR-1477](../adr/1477-speed-upstream-double-math.md) | Yes | `speed_chroma.cuda`, `speed_temporal.cuda` |
| SYCL | `speed_chroma_sycl`, `speed_temporal_sycl` | [ADR-1358](../adr/1358-sycl-speed-device-resident-linalg.md), ADR-1477 | Yes (except an AdaptiveCpp build: it lacks the correctly rounded division fallback and keeps the ADR-0214 tolerance) | `speed_chroma.sycl`, `speed_temporal.sycl` |
| HIP | `speed_chroma_hip`, `speed_temporal_hip` | [ADR-1384](../adr/1384-hip-speed-device-resident.md), ADR-1477 | Yes | `speed_chroma.hip`, `speed_temporal.hip` |

Metal has no SpEED twin.

### The parity gate compares both scores

`speed_chroma` and `speed_temporal` are features of the cross-backend parity
gate ([gate guide](../development/cross-backend-gate.md)). `speed_temporal`
joined on 2026-10-02
([ADR-1460](../adr/1460-gate-speed-temporal-and-uncovered-twins.md)); before,
its three twins were covered by their own unit tests only. All six cells (two
features, three backends) are exact: the gate compares them at
`--precision max` with tolerance `0`
([ADR-1477](../adr/1477-speed-upstream-double-math.md)).

### Where a twin computes what {#where-a-twin-computes-what}

A twin runs every stage of `speed.c` up to the solved linear system on the
device:

1. Picture conversion (and the frame difference of `speed_temporal`).
2. The optional prescale.
3. The anti-alias filter at the decimated sample points.
4. Local mean subtraction.
5. The 25x25 covariance and its eigenvalues.
6. The regularity decision and the QR solve, which gives one variance per 5x5
   block.

It reads one block back per frame: per channel two status words, the 25
eigenvalues and the variances. The host then forms the entropies and the score
with `speed.c`'s own statements (`speed_internal_gpu_tail_scores()` in
`core/src/feature/speed_internal.c`).

That last step is on the host because it is where `speed.c` calls the C
library: `log2()` 25 times per block and channel, and once more per block for
the score.

No device has the host's `log2` (glibc's and Intel's differ from each other,
and neither is what a device computes), so a twin that evaluated it itself
could only be close. On the host the twin makes the calls the CPU extractor
makes, in the same library. The cost is 38 to 380 microseconds per frame on a
Ryzen 9 9950X3D (1920x1080 `speed_chroma` to 3840x2160 `speed_temporal`).

One fp64 statement stays on the device, the Givens rotation's
`1.0 / sqrt(1 + t * t)`. The kernels have no fp64 type; they compute it from
fp32 operations (`core/src/feature/speed_givens.h`), and
`test_speed_upstream_form` compares that routine with the fp64 statement on
every input it can receive, all 8,388,609 floats of [1, 2].

#### Measured agreement

At `--precision max` against `--backend cpu` of the same build, on an RTX 4090
and a gfx1036 (GCC build, glibc 2.44) and on an Arc A380 (icx build, Intel's
math library):

- **Default options:** 759 of 759 `speed_chroma` values and 256 of 256
  `speed_temporal` values, on the Netflix 576x324 pair at four bit depths and
  in three chroma formats, both 1080p checkerboard pairs, Sparks, noise at four
  bit depths, a bright 16-bit pair, two gradients and BBB at 1920x1080 and
  3840x2160.
- **18 option sets:** 2052 of 2052 and 342 of 342 values (the `vmaf_v1.0.16`
  options, every weighting mode, kernelscale, `speed_sigma_nn`,
  `speed_nn_floor`, prescale down and up with four methods,
  `speed_use_ref_diff`).

### Speed

Milliseconds per frame, `(t(N) - t(2)) / (N - 2)` through the `vmaf` tool,
median of 3 (the CPU rows are `--backend cpu --threads 16`, which runs frames
in parallel; the GPU rows run one frame at a time). "Before" is before the
device-resident pipeline of the ADR in the first column.

| Twin and device | Feature | Size | Before | After | CPU (16 threads) |
| --- | --- | --- | ---: | ---: | ---: |
| SYCL, Arc B580 (ADR-1358) | `speed_chroma` | 576x324 | 3.60 | 0.89 | 0.16 |
| SYCL, Arc B580 | `speed_chroma` | 3840x2160 | 23.31 | 7.51 | 7.24 |
| SYCL, Arc B580 | `speed_temporal` | 576x324 | 2.19 | 0.83 | 0.85 |
| SYCL, Arc B580 | `speed_temporal` | 3840x2160 | 60.37 | 7.58 | 37.27 |
| SYCL, UHD 770 (ADR-1358) | `speed_chroma` | 576x324 | 17.31 | 3.44 | 0.16 |
| SYCL, UHD 770 | `speed_chroma` | 3840x2160 | 37.98 | 14.48 | 7.24 |
| SYCL, UHD 770 | `speed_temporal` | 576x324 | 8.32 | 3.47 | 0.85 |
| SYCL, UHD 770 | `speed_temporal` | 3840x2160 | 72.15 | 18.27 | 37.27 |
| CUDA, RTX 4090 (ADR-1380) | `speed_chroma` | 3840x2160 | 24.90 | 6.89 | 9.11 |
| CUDA, RTX 4090 | `speed_chroma` | 576x324 | 1.51 | 0.44 | 0.12 |
| CUDA, RTX 4090 | `speed_temporal` | 3840x2160 | fails | 5.88 | 26.74 |
| CUDA, RTX 4090 | `speed_temporal` | 576x324 | 2.81 | 0.42 | 0.92 |

The SYCL rows are one Arc B580 and one UHD 770 through WSL2 Level Zero, on an
i9-12900K, with the icx/icpx 2026.1 build of the `vmaf-dev-mcp` image. At
576x324 the startup-free difference is within run-to-run noise, so those rows
use 48 frames and 5 repetitions. The CUDA rows are before and after in
alternation on the same host
([Research-1379](../research/1379-cuda-cambi-speed-device-resident.md) has the
method and the raw numbers).

#### What holds across runs

- **SYCL at 4K.** Both twins run at about the rate the CLI reads 4K frames from
  disk; `speed_temporal`, which the CPU cannot run frames in parallel for, is
  about five times faster than the CPU.
- **SYCL at 576x324.** The eigenvalue sweep, which runs on one work item for
  the reference's operation order, keeps the twins at about a millisecond per
  frame on the B580.
- **CUDA at 4K.** The twins cost about what the trivial `psnr_cuda` costs. At
  load average 13, with N = 102, `psnr_cuda` took 2.41 to 2.56 ms per frame,
  `speed_chroma_cuda` 2.75 and `speed_temporal_cuda` 2.73: reading and
  uploading the pictures, not the SpEED kernels, sets their speed. Absolute
  numbers move with the host's load.
- **HIP.** `speed_chroma_hip` with lanczos4 at 0.5 went from 21.8 to 16.0 ms
  per 3840x2160 frame (see [below](#lanczos4-prescale)).

Before ADR-1359, the CPU name ran the CPU extractor on one thread under
`--backend sycl` (18.35 ms per frame at 4K on the SYCL machine above); the
rows request the twin by its registered name.

### SYCL: device-resident and bit-identical to the CPU {#sycl-device-resident}

Since [ADR-1358](../adr/1358-sycl-speed-device-resident-linalg.md),
`speed_chroma_sycl` and `speed_temporal_sycl` run the whole per-frame chain on
the device (the stages [above](#where-a-twin-computes-what)). Each frame costs
one upload of the raw planes and one read of the result block; the host no
longer filters, factorises or waits in between, and forms the entropies and the
score after its one wait. The chain is recorded once as a SYCL graph and
replayed per frame where the device supports graphs.

Every stage reproduces the CPU extractor's arithmetic in fp32, so the SYCL
scores are not merely within tolerance. On the Netflix 576x324 pair (48 frames)
and 50 frames of BBB 3840x2160, every per-frame `speed_chroma_u`,
`speed_chroma_v`, `speed_chroma_uv` and `speed_temporal` value is identical to
`--backend cpu` at `--precision max`, on an Arc B580 and on a UHD 770
(2026-09-29, an icx build; an Arc A380 since ADR-1477).

```sh
vmaf -r ref.yuv -d dis.yuv -w 3840 -h 2160 -p 420 -b 8 --no_prediction \
  --backend sycl --feature speed_chroma_sycl -o out.json --json
```

### CUDA: the same chain on the device

Since [ADR-1380](../adr/1380-cuda-speed-device-resident-pipeline.md),
`speed_chroma_cuda` and `speed_temporal_cuda` run the SYCL chain on CUDA, in
one pipeline both extractors share.

- They take their planes from the picture the CUDA engine already uploaded,
  with device-to-device copies (`speed_temporal` keeps the previous frame's
  luma on the device).
- They read back one result block per frame; `collect()` is the only wait.
- Every rounding the CPU performs is spelled with a round-to-nearest
  intrinsic (`__fadd_rn`, `__fmul_rn`, `__fdiv_rn`, `__fsqrt_rn`, ...), which
  nvcc never fuses, and the kernels are also built with `--fmad=false`.

```sh
vmaf -r ref.yuv -d dis.yuv -w 3840 -h 2160 -p 420 -b 8 --no_prediction \
  --backend cuda --feature speed_chroma_cuda -o out.json --json
```

On an RTX 4090 at `--precision max`, every per-frame `speed_chroma_u`,
`speed_chroma_v`, `speed_chroma_uv` and `speed_temporal` value is identical to
`--backend cpu` of the same GCC build, and so are nearest, bilinear, bicubic and
`lanczos4` prescale (at 0.5 and 2.0).

### HIP: device-resident, CPU fp32 arithmetic {#hip-device-resident-cpu-fp32-arithmetic}

Since [ADR-1384](../adr/1384-hip-speed-device-resident.md), `speed_chroma_hip`
and `speed_temporal_hip` run the same chain as the SYCL twins: one staged upload
of the raw planes (the host copies them into pinned memory and returns without
waiting), seven kernels and one read of the result block; `collect()` is the
only wait. The geometry, filter taps and scoring options come from
`speed_internal_gpu_configure()`, the routine the SYCL twins use too.

HIP needs its exact arithmetic spelled out per build, not per operation:

- The kernel file is compiled with `-ffp-contract=off` (hipcc otherwise fuses
  `a * b + c` into an FMA) and `-fhip-fp32-correctly-rounded-divide-sqrt`, and
  the code uses plain `/` and `sqrtf()`.
- HIP's `__fmul_rn()`, `__fadd_rn()` and `__fdiv_rn()` are the plain
  operators, and `__fsqrt_rn()` is the approximate native square root unless
  the device library is built with `OCML_BASIC_ROUNDED_OPERATIONS`, so they
  would not round correctly.

### lanczos4 prescale

`speed_prescale_method=lanczos4` is exact on all three twins. The CPU evaluates
each kernel weight with fp64 `sin` and rounds it once. The weights depend only
on the output column and row, so the host evaluates them once per run with the
CPU scaler's own routine (`vif_scale_lanczos4_axis_weights()` in `vif_tools.c`)
and the scale kernel reads the table from device memory.

| Twin | Before the host table | Now |
| --- | --- | --- |
| CUDA | A device `sin` in fp32 was a few ulp off on some weights, which SpEED amplified to as much as 8.8e-3 relative on a smooth synthetic field with `speed_prescale=0.5` (`T-GPU-SPEED-LANCZOS4-PRESCALE-DRIFT-2026-09-30`). | Nearest, bilinear, bicubic and lanczos4 at 0.5 and 2.0 identical to the CPU. |
| SYCL | 5.8e-5 relative on an Arc A380 at 2.0, on a smooth field with scores near 20 where the ADR-0214 tolerance is 1e-4. | On an Arc A380, the CPU's value on every frame of a 1920x1080 gradient with noise, the Netflix pair, both 1080p checkerboard pairs, a 10-bit 720p gradient and BBB 3840x2160; the scale kernels use no scratch memory ([ADR-1395](../adr/1395-sycl-kernels-no-scratch.md)). |
| HIP | The twins evaluated the weights in fp32 (`sinpif`). On a gfx1036, 228 of 504 frame values were identical and the worst was 0.238 away (`speed_temporal` at 2.0 on a checkerboard pair). | All 504 identical. |

The HIP measurement is on a gfx1036 against `--backend cpu` at
`--precision max`, lanczos4 at `speed_prescale` 0.5 and 2.0, `speed_chroma` and
`speed_temporal`, on the Netflix 576x324 pair (48 frames, and 3 frames at 10
bits), both 1080p checkerboard pairs and BBB 3840x2160 (6 frames).
`test_hip_speed_device_math` replays the kernels with the table against the CPU
extractor without a device, and `test_hip_speed_lanczos4_parity` runs the twins
on one.

### The CPU reference and the host's `log2` {#the-cpu-reference-and-log2f}

The CPU extractor calls `log2` from the platform's C library about 25 times per
block and rounds each sum to `float`. C libraries do not agree on the last bit
of an fp64 `log2`, but the rounding to `float` absorbs it: GCC and clang builds
on glibc for x86-64 and aarch64 return the same `speed_chroma` and
`speed_temporal` scores on every clip compared with Netflix, and an icx build
with Intel's library the same 3409 values as a GCC build on the clips and
option sets of the twin comparison.

A twin does not depend on that agreement: its logarithms are the host's, so
`--backend cpu` and a GPU backend of one binary return the same bits whatever
the library is.

!!! warning "Build the CPU reference without FMA contraction"
    The CPU build must not fuse multiply-adds. icx does when FMA instructions
    are available, for example with `-march=native`, which is how the
    `vmaf-dev-mcp` image builds its own `/usr/local/bin/vmaf`: that binary's
    SpEED scores are up to 7.9e-4 from a default build at 1080p. Compare the
    GPU twins against a build without `-march=native`
    ([Research-1379](../research/1379-cuda-cambi-speed-device-resident.md)).

### Checking a GPU twin against the CPU

`scripts/dev/speed_gpu_parity.py` runs the CPU extractor and a GPU twin over two
fixtures, prints the bit-identical frame count and the largest difference per
output, then times both the same way as the table. It exits 0 only when every
output of every frame is identical:

```sh
python3 scripts/dev/speed_gpu_parity.py --backend cuda \
  --vmaf build/tools/vmaf --netflix-dir python/test/resource/yuv --bbb-dir testdata/bbb
# SYCL: pick the device first, e.g. ONEAPI_DEVICE_SELECTOR=level_zero:0
```

- **Fixtures.** The Netflix 576x324 pair from `--netflix-dir`, and a Big Buck
  Bunny 3840x2160 8-bit 4:2:0 pair that you supply as
  `ref_3840x2160_200f.yuv` and `dis_3840x2160_200f.yuv` in `--bbb-dir`. The
  default `testdata/bbb` is not tracked in the repository, so pass `--bbb-dir`.
- **Options.** `--no-timing` skips the timing runs; `--reps` and `--threads`
  change the repetitions and the CPU thread count. `--max-abs-diff` sets a
  bound instead of bit identity.
- **Binary.** `--vmaf` takes an absolute path, a path relative to the working
  directory (the default is `build/tools/vmaf`), or a bare name to look up on
  `PATH`. The CPU side runs from the same `vmaf` binary; a GCC build and an icx
  build both compare exactly.

## Python compat wrappers

The compat Python harness (`compat/python-vmaf/`) ships Python wrappers for
both extractors, ported from Netflix upstream per the Research-0732 audit
(PR #22):

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
`--feature speed_temporal` respectively and parse the resulting XML log. The C
extractors must have been compiled with `-Denable_float=true` (the default).

## Test coverage

- `core/test/test_speed_upstream_form`: Netflix's statements and values (see
  [above](#checking-a-build-against-netflixs-values)).
- `core/test/test_speed_simd`: the SIMD kernels against the scalar reference.
- `core/test/test_{cuda,sycl,hip}_speed_{chroma,temporal}_parity`: the twins
  against the CPU with `==`.
- `core/test/test_{cuda,sycl,hip}_speed_singular_parity`
  ([ADR-1218](../adr/1218-gpu-speed-singular-device-solution.md)): the
  singular-matrix path.
- `core/test/test_{cuda,sycl,hip}_speed_lanczos4_parity`: the lanczos4 weights.
- `core/test/test_speed`: CPU registration and behaviour, in a build with
  `-Denable_float=true`.

## History

### SpEED upstream parity

- **2026-10-02, ADR-1477 fp64 forms.** Until 2026-10-02 the fork's port had
  three fp32 forms where Netflix computes in fp64 (`1.0f / sqrtf()` in the
  Givens rotation, `log2f()` in the entropy and in the score). Scores from
  before that differ from Netflix's and from today's by up to 2.3e-5
  (`speed_chroma`), 6.6e-4 (`speed_temporal`) and 2.5e-5 (`vmaf_v1.0.16`); see
  [ADR-1477](../adr/1477-speed-upstream-double-math.md).
- **2026-10-02, ADR-1477 twin bounds removed.** The twins were bounded at `5e-6`
  and `4e-5` ([ADR-1430](../adr/1430-cuda-speed-chroma-log2f-bound.md)) while
  the CPU called `log2f`, which glibc rounds the wrong way for 0.14 % of the
  floats in [1, 1024), and the twins rounded `log2` correctly on the device.
- **2026-10-02, ADR-1477 measured differences.** A twin then matched an icx
  build's CPU and differed from a GCC build's on a few
  outputs, by one to five steps of the fp32 score: 13 of 789 `speed_chroma`
  values on an RTX 4090
  ([Research-1430](../research/1430-cuda-speed-chroma-log2f-bound.md)), 13 of
  990 on a gfx1036, 15 of 918 on an Arc A380, 2 of 104 `speed_temporal` frames.
  Scores stored from a GPU run before 2026-10-02 carry those forms.

### Singular covariance and non-finite scores

- **Singular covariance on the GPU twins**
  ([ADR-1218](../adr/1218-gpu-speed-singular-device-solution.md)). Up to and
  including v3.2.1 the twins diverged from the CPU on two counts, described in
  the next two entries.
- **Singular covariance: `speed_temporal` twins.** The three `speed_temporal`
  twins never reported singularity to their caller, so the one-sided rule did
  not exist there and they returned the score kernel's result. On a 960x960
  fixture with a frozen reference and a moving distorted side the CPU returns
  `0.00000000` and every GPU backend returned `230.71379089`. Re-measure any
  GPU `speed_temporal` score taken over content with static passages.
  (`speed_chroma` was fixed earlier, in
  [ADR-1202](../adr/1202-cuda-speed-chroma-4k-launch-bounds.md).)
- **Singular covariance: stale device solution.** All six twins zeroed a *host*
  staging buffer on the singular path and uploaded nothing, so the score kernel
  read the device solution left over from the previous frame, or on the first
  frame whatever the allocator returned. This has no demonstrable effect on the
  emitted score, because when both sides are singular the CPU's own zeroed
  solution drives every block's variance to `0` and the score to exactly `0`
  regardless; it is fixed because reading uninitialised device memory is
  undefined behaviour.
- **Non-finite scores**
  ([ADR-1301](../adr/1301-speed-nonfinite-score-fails-frame.md)).
  The score used to be bounded with a less-than comparison against
  `speed_max_val`, and every comparison against NaN is false, so a NaN was
  emitted as `speed_max_val` itself: a finite, plausible 1000.0 that no caller
  could distinguish from a real measurement. `-Inf` passed the comparison and
  was published unclamped.

### Device-resident pipelines

- **Two earlier algorithm defects in the GPU kernels, corrected.** The
  mean/covariance kernels now compute a single covariance over the full
  phase-shifted 5x5 submatrix (a `means[25]` window), matching the CPU
  reference; the previous kernels computed per-tile, block-local statistics,
  which understated the score roughly seven-fold. The reference and distorted
  entropy terms now use independent covariance and eigenvalue bases; the
  previous kernels reused the reference basis for the distorted plane, biasing
  the chroma score high whenever the reference and distorted frames differed.
- **ADR-1358, ADR-1380, ADR-1384.** Before ADR-1358 1 to 9 SYCL frames per run
  matched and the largest difference was 4.2e-5. Before ADR-1380, 1 to 10
  CUDA frames per output matched, and `speed_temporal_cuda` failed at
  1920x1080 and above with `CUDA_ERROR_INVALID_VALUE`.
- **Lanczos4 weights on the host**
  (`T-GPU-SPEED-LANCZOS4-PRESCALE-DRIFT-2026-09-30`,
  see [lanczos4 prescale](#lanczos4-prescale)).

## See also

- [SpEED-QA](speed_qa.md): the no-reference `speed_qa` feature.
- [Feature overview](features.md): the full extractor table.
- [CUDA backend](../backends/cuda/overview.md) and
  [SYCL backend](../backends/sycl/overview.md): backend selection and
  `--backend`.
