# VIF

VIF (Visual Information Fidelity) measures how much of the reference image's
visual information is preserved in the distorted image, using a statistical
model of natural scenes and human visual system sensitivity. Scores are per
scale of a four-level pyramid; higher values mean more of the reference
information is preserved.

VIF is a **luma-only metric by design** (Sheikh & Bovik, 2006, defined on a
single luminance channel). Every libvmaf backend (CPU with AVX2, AVX-512 and
NEON; CUDA; HIP; SYCL; Metal) and upstream Netflix/vmaf reads only the Y plane
(`data[0]`) of the input picture. There are no per-chroma-plane VIF features.
See [ADR-0597](../adr/0597-integer-vif-luma-only-clarification.md) for the
disposition of the 2026-05-18 deep-audit finding that questioned this design.

## Variants

| Registered name | Algorithm | Output features |
| --- | --- | --- |
| `vif` | Integer fixed-point multi-scale implementation. This is the extractor VMAF model JSON files reference. | `integer_vif_scale0..3` (collector keys `VMAF_integer_feature_vif_scaleN_score`) |
| `float_vif` | Floating-point 4-scale Gaussian-pyramid implementation. | `vif_scale0..3` (collector keys `VMAF_feature_vif_scaleN_score`) |

The registered name of the integer extractor is `vif`; `integer_vif` is not
accepted by `--feature` (it fails with
`problem loading feature extractor: integer_vif`).

## How to run

```bash
# Integer VIF, luma only
build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature vif --output /dev/stdout

# Skip scale-0 (GPU-parity mode)
build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature vif=vif_skip_scale0=true --output /dev/stdout

# Floating-point VIF
build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature float_vif --output /dev/stdout
```

Options follow the feature name after the first `=`, separated by `:`. An
option with a non-default value is appended to the output key as its alias, for
example `integer_vif_scale0_ssclz` for `vif_skip_scale0=true`.

## Output features

All features are computed on the luma (Y) plane only. The integer extractor
emits these (the float extractor uses the same names without the `integer_`
prefix):

| Feature name | Description | Condition |
| --- | --- | --- |
| `integer_vif_scale0` | VIF at scale 0 (finest, luma) | Always (or `0.0` when `vif_skip_scale0=true`) |
| `integer_vif_scale1` | VIF at scale 1 (luma) | Always |
| `integer_vif_scale2` | VIF at scale 2 (luma) | Always |
| `integer_vif_scale3` | VIF at scale 3 (coarsest, luma) | Always |
| `integer_vif` | Fused VIF score (luma) | `debug=true` |
| `integer_vif_num`, `integer_vif_den` | Fused numerator/denominator | `debug=true` |
| `integer_vif_num_scaleN`, `integer_vif_den_scaleN` (N=0..3) | Per-scale num/den | `debug=true` |

## Options

| Option | Alias | Extractor | Type | Default | Range | Effect |
| --- | --- | --- | --- | --- | --- | --- |
| `vif_enhn_gain_limit` | `egl` | both | double | `100.0` | `1.0–100.0` | Cap the per-pixel enhancement-gain ratio so over-sharpened content cannot saturate the score. Set to `1.0` to disable enhancement gain entirely. |
| `vif_skip_scale0` | `ssclz` | both | bool | `false` | n/a | Skip scale-0 (finest pyramid level) calculations; scale-0 outputs are forced to `0.0` and excluded from the fused score. Matches the CPU path for GPU backends. |
| `debug` | none | both | bool | `false` | n/a | Emit additional per-scale numerator/denominator debug metrics. |
| `vif_fused` | none | `vif_sycl` only | bool | `false` | n/a | Run each scale's vertical and horizontal filter passes in one kernel launch. It needs about 230 MB less device memory at 3840x2160 and took about the same time per frame on an Arc A380. The scores are the CPU's either way; they are reported as `integer_vif_scale0_vif_fused` to `integer_vif_scale3_vif_fused`. |
| `vif_kernelscale` | `ks` | `float_vif` | double | `1.0` | `0.1` to `4.0` | Scale of the Gaussian kernel (2.0 doubles the standard deviation and lengthens the kernel). |
| `vif_prescale` | `ps` | `float_vif` | double | `1.0` | `0.1` to `4.0` | Scaling factor for the frame (2.0 doubles width and height). |
| `vif_prescale_method` | `pm` | `float_vif` | string | `nearest` | `nearest`, `bilinear`, `bicubic`, `lanczos4` | Scaling method for the prescale. |
| `vif_scale1_min_val`, `vif_scale2_min_val`, `vif_scale3_min_val` | `s1miv`, `s2miv`, `s3miv` | `float_vif` | double | `0.0` | `0.0` to `1.0` | A scale's score below its floor is reported as the floor. |
| `vif_sigma_nsq` | `snsq` | `float_vif` | double | `2.0` | `0.0` to `5.0` | Neural noise variance. |
| `enable_chroma` | none | `vif_cuda` only | bool | `false` | n/a | **No-op** retained for backward compatibility with callers that pass the option on the CLI or in a model JSON. VIF is luma-only by design; `enable_chroma=true` emits a one-shot warning during init (`integer_vif (CUDA): enable_chroma=true requested but VIF is luma-only by design (...); option is a no-op. See ADR-0597.`) and leaves the scores unchanged. The scores are reported under names with the option's suffix, as for any option the caller sets: `integer_vif_scale0_enable_chroma` to `integer_vif_scale3_enable_chroma` (in the collector and `--output`), since [ADR-1836](../adr/1836-cuda-vif-enable-chroma-names.md); before it they kept the default `integer_vif_scale0` to `integer_vif_scale3` names. |

Option aliases are part of the published collector key. Equivalent GPU twin
options use the CPU spellings (`ks`, `ssclz` and `egl`) so backend selection
does not rename a feature
([ADR-1312](../adr/1312-gpu-option-alias-parity.md)).

## Minimum frame size

`float_vif` runs a four-scale pyramid: each scale halves the working dimension
and then convolves at that size with the scale's own separable Gaussian: 17, 9,
5 and 3 taps at the default `vif_kernelscale` of 1.0. Every one of those
convolutions needs at least `filter_width / 2 + 1` samples in each axis for the
reflect-101 mirror padding to stay inside the plane, so the frame minimum is the
largest `(filter_width_s / 2 + 1) << s` over the ladder:

| Scale | Filter width | Needs at that scale | Implies at full size |
| --- | --- | --- | --- |
| 0 | 17 | 9 | 9 |
| 1 | 9 | 5 | 10 |
| 2 | 5 | 3 | 12 |
| 3 | 3 | 2 | **16** |

**`float_vif` therefore rejects any frame below 16x16** with `-EINVAL` at
`init()`:

```text
libvmaf ERROR float_vif requires width >= 16 and height >= 16 for the
four-scale ladder (got 12x12)
```

- The bound is derived from `vif_kernelscale`, not hard-coded, so a
  non-default kernel scale moves it.
- When `vif_prescale != 1.0` the check is applied to the *scaled* dimensions
  (the ones actually handed to the pyramid) as well as to the raw input.
- The integer pyramid has the same bound: every scale reflects its filter taps
  once, which stays inside the plane only from 16 pixels in each dimension up.

### Largest prescaled plane

`float_vif` hands the prescaled plane to the functions of
`core/src/feature/vif_tools.c`, which index it with `int`, so the plane must
have at most INT_MAX (2,147,483,647) samples, counted with its row stride.
16K (15360x8640) stays inside at every accepted `vif_prescale`, up to 4.0
(2,123,366,400 samples); at the 32768x32768 picture cap the limit is a
prescale of about 1.414. A larger plane fails `init()` with `-EINVAL`:

```text
libvmaf ERROR float_vif: the prescaled plane (65536x65536) has more samples than the int index of vif_tools allows; lower vif_prescale
```

`core/test/test_prescaled_plane_int_index.c` checks the limit without
allocating a plane of that size.

### Minimum frame size on the integer GPU twins

For the integer GPU twins:

- **Declared minimum.** `vif_sycl`, `vif_cuda` and `vif_hip` declare it
  ([ADR-1374](../adr/1374-cuda-integer-tiny-frame-guards.md) for CUDA,
  [ADR-1381](../adr/1381-hip-integer-tiny-frame-guards.md) for HIP).
- **Smaller frame under model dispatch.** Under model dispatch, and for
  `--backend sycl|cuda|hip` with `--feature vif`, a smaller frame is computed by
  the CPU `vif` and matches it bit for bit.
- **Named twin.** Naming the twin (`--feature vif_cuda`, `--feature vif_hip`) on
  such a frame fails at `init()` with `-EINVAL` and a message such as
  `vif_cuda requires width >= 16 and height >= 16`.
- **Metal.** The Metal twin does not declare it yet
  (`T-GPU-INTEGER-VIF-MIN-DIM-TWINS-2026-09-29`).

## GPU twins

Every GPU twin gives the CPU's value for every output of every frame, down to
the last bit of the `--precision max` output, except Metal, which agrees with
the CPU to four decimal places (`places=4`,
[ADR-0214](../adr/0214-gpu-parity-ci-gate.md)). The exactness is declared in
`scripts/ci/exact_twins.d/`.

| Twin | Extractor | Exact vs CPU | ADR | Evidence (fragment) | Measured on | Previous max difference |
| --- | --- | --- | --- | --- | --- | --- |
| `vif_cuda` | `vif` | Yes | [ADR-1462](../adr/1462-cuda-vif-reads-host-log2-table.md) | `vif.cuda` | RTX 4090: 1392 of 1392 scores on 348 frames from 40x40 to 3840x2160 at 8 to 16 bits | not recorded |
| `vif_sycl` | `vif` | Yes | [ADR-1432](../adr/1432-sycl-integer-vif-exact-gain.md) | `vif.sycl` | Arc A380: Netflix 576x324 at 8, 10, 12 and 16 bits and 4:2:2, both 1920x1080 checkerboard pairs, 200 frames at 3840x2160 | 3.6e-7 |
| `vif_hip` | `vif` | Yes | [ADR-1435](../adr/1435-hip-vif-cpu-log2-table.md) | `vif.hip` | gfx1036: 440 of 440 scores from 480x270 to 3840x2160 | not recorded |
| `integer_vif_metal` | `vif` | No (`places=4`) | ADR-0214 | none | macOS Apple-Silicon CI lane | not applicable |
| `float_vif_cuda` | `float_vif` | Yes | [ADR-1412](../adr/1412-cuda-float-vif-cpu-arithmetic.md) | `float_vif.cuda` | RTX 4090: Netflix 576x324 at 8, 10, 12 and 16 bits, both 1920x1080 checkerboard pairs, 3840x2160 | 3.8e-5 (the fourth decimal place of `vif_scale3` could differ) |
| `float_vif_sycl` | `float_vif` | Yes | [ADR-1422](../adr/1422-sycl-float-vif-cpu-arithmetic.md) | `float_vif.sycl` | Arc A380: Netflix 576x324 at 8, 10, 12 and 16 bits, both 1920x1080 checkerboard pairs, 200 frames at 3840x2160 | 3.8e-5 |
| `float_vif_hip` | `float_vif` | Yes | [ADR-1444](../adr/1444-hip-float-vif-cpu-arithmetic.md) | `float_vif.hip` | gfx1036 (ROCm 7.2.4): 712 scores, Netflix 576x324 at 8, 10, 12 and 16 bits and as 10-bit 4:2:2, both 1920x1080 checkerboard pairs, Sparks 480x270, full-range noise at four bit depths, a bright 16-bit 1920x1080 pair and 48 frames at 3840x2160 | 3.8e-5 on typical content, 1.06e-4 on bright 16-bit content |
| `float_vif_metal` | `float_vif` | No (`places=4`) | ADR-0214 | none | macOS Apple-Silicon CI lane | not applicable |

The exact twins also match with `debug=true` (and, for `float_vif_hip`, with
the feature options).

### What exactness means when you use it

`--backend cpu --feature vif` and a GPU twin return the same number for every
output of every frame, and so do `--backend cpu --feature float_vif` and the
`float_vif` twins (Metal excepted).

- You can mix CPU and GPU `vif` and `float_vif` results in one data set. Output
  stored before the ADR in the table differs from new output by the "previous
  max difference" figure.
- Check a twin on your own device with the parity script. It prints, per
  output, how many frames are bit-identical and the largest difference, and
  exits 0 only when every frame is:

```shell
python3 scripts/dev/speed_gpu_parity.py --backend cuda \
    --vmaf "$PWD/build/tools/vmaf" --feature float_vif
# vif twins: --feature vif. SYCL: prefix with ONEAPI_DEVICE_SELECTOR=level_zero:0.
# HIP: --backend hip --vmaf "$PWD/build-hip/tools/vmaf".
```

### Options on the `float_vif` twins

- **Floors.** `float_vif_cuda`, `float_vif_sycl` and `float_vif_hip` accept the
  per-scale floors of the CPU extractor (`vif_scale1_min_val`,
  `vif_scale2_min_val`, `vif_scale3_min_val`; default 0, range 0 to 1).
- **Prescale.** `vif_prescale` and `vif_prescale_method` remain CPU-only.
  Passing either to a twin is an error; request `float_vif` for prescaled
  scoring.
- **Kernel scale.** The GPU `float_vif` kernels implement `vif_kernelscale=1.0`
  only. When a model requests another valid value, libvmaf automatically
  selects the CPU `float_vif` extractor for that feature before GPU
  initialization; unrelated model features remain on their selected backends.
  Explicitly naming a GPU extractor with a non-default kernel scale still
  returns `-EINVAL`, because that form requests the specific extractor rather
  than automatic model dispatch. See
  [ADR-1316](../adr/1316-gpu-option-value-capability-fallback.md).
- **Noise variance and gain limit.** `vif_sigma_nsq` and `vif_enhn_gain_limit`
  are honoured on every backend (see the warning below).

!!! warning "Re-score NEG runs made on a GPU before ADR-1217"
    Up to and including v3.2.1 the CUDA, SYCL and HIP `float_vif` kernels
    silently ignored a non-default `vif_sigma_nsq` or `vif_enhn_gain_limit`, so
    the NEG model scored on a GPU produced ordinary, non-NEG values under the
    NEG feature keys.

The kernels hardcoded the two defaults (`vif_sigma_nsq = 2.0`,
`vif_enhn_gain_limit = 100.0`) as local constants: a non-default value was
accepted, range-checked, folded into the derived feature name, and then ignored.
[`model/vmaf_float_v0.6.1neg.json`](https://github.com/VMAFx/vmafx/blob/master/model/vmaf_float_v0.6.1neg.json)
sets `vif_enhn_gain_limit = 1.0` on all four VIF scales (that setting *is* what
makes it the NEG model), so running it on a GPU backend produced non-NEG scores
published under the NEG feature keys. If you scored with the NEG model on CUDA,
SYCL or HIP before the fix, re-score: those numbers were the ordinary
enhancement-gain-enabled VIF. Fixed per
[ADR-1217](../adr/1217-gpu-float-vif-options-reach-kernel.md).

### Selecting a twin

- **By model or `--backend`.** For `vif`, `--backend sycl|cuda|hip` with
  `--feature vif` runs the twin.
- **HIP `float_vif`.** `--backend hip --feature float_vif` runs
  `float_vif_hip`, and so does a VMAFx context on a HIP device
  ([ADR-2092](../adr/2092-vmafx-hip-device-frames.md)). A build with
  `-Denable_float_vif_hip_autodispatch=false` (on by default) prints
  `the hip backend has no twin of this extractor; computing it on the CPU`
  instead and runs the CPU extractor; the twin then runs only when named
  (`--feature float_vif_hip`).

### Integer twin notes

- **`vif_sycl`, what used to differ.** The integer extractor stores each scale's
  numerator and denominator sum in a `float` and divides in single precision,
  where `vif_sycl` kept the sums in `double`. The integer extractor also
  computes a pixel's gain in `double` and truncates two results to integers,
  where `vif_sycl` used `float`, which put a share of those integers one off.
  It now computes both integers exactly, in integer arithmetic.
- **`vif_sycl`, `vif_fused=true`.** Until October 2026 scales 1 to 3 differed
  from the CPU on every frame from 1920x1080 up (by up to 4.9e-4 at
  3840x2160): one launch read a scale from the buffers it was writing the
  next scale into. The fused scales now alternate between two buffers, and
  the scores equal the CPU's and the separate passes'.
- **`vif_sycl`, `debug`.** The option defaulted to `true` until October 2026,
  which added the eleven debug outputs to every run; request them with
  `--feature vif_sycl=debug=true`.
- **`vif_sycl`, gain limit.** With a `vif_enhn_gain_limit` that is not a whole
  number, a pixel whose gain reaches the limit takes a slower exact path. The
  scores are the CPU's, and the default (100) and the NEG value (1) are not
  affected.
- **`vif_hip` and `vif_cuda`.** Their kernels read the log2 table the CPU
  extractor builds instead of computing logarithms on the device (for CUDA it
  is uploaded once when the extractor starts), so the equality holds whatever
  math library the host and the device toolkit have. See the
  [HIP backend page](../backends/hip/twins.md#vif_hip)
  and the
  [CUDA backend page](../backends/cuda/overview.md#vif_cuda-returns-the-cpus-scores-bit-for-bit-2026-10-02).

### Float twin notes

- **`float_vif` on SYCL.** SYCL kernels have no 64-bit floating-point type, so
  the two expressions the CPU evaluates in `double` are computed in pairs of
  floats and, for the rare sample next to a rounding boundary, in integers.
- **`float_vif` on CUDA, SYCL and HIP.** They filter with the Gaussian taps the
  CPU extractor computes at start-up, evaluate the CPU's per-pixel statistic in
  the CPU's types (including its polynomial `log2`), and add the per-pixel
  terms in the CPU's order: one `float` sum per row, then the rows. The HIP
  twin runs the CUDA twin's arithmetic from the same source file.

### Metal twin notes

- **`integer_vif_metal`.** A 4-scale fixed-point Gaussian pyramid
  (`feature/metal/integer_vif.metal` + `_metal.mm`) with int64 moment
  accumulators and the CPU's log2 table. Apple GPUs have no fp64, so the two
  integers the CPU truncates from its double gain come from an integer
  division, with the CPU's double operations replayed in 64-bit integers next
  to an integer boundary (`metal_integer_vif_gain.h`, the SYCL twin's method,
  [ADR-1498](../adr/1498-metal-twins-exact-designs.md)). The host rounds each
  scale's sums to `float` and divides them in single precision, as
  `integer_vif.c` does.
- **`float_vif_metal`.** A 4-scale separable-Gaussian pyramid with per-scale
  mean/variance/covariance statistics (`feature/metal/float_vif.metal` +
  `_metal.mm`).

### Cost of exactness

| Twin | Frame | Time now | Before | CPU extractor |
| --- | --- | --- | --- | --- |
| `vif_sycl`, Arc A380 | 3840x2160 | 22.2 ms (through the `vmaf` tool) | 21.5 ms | not recorded |
| `float_vif_sycl`, Arc A380 | 3840x2160 | 24.0 ms | 20.5 ms (before ADR-1422) | 68 ms on 16 threads |
| `float_vif_hip`, gfx1036 | 1920x1080 | 26.0 ms | 20.7 ms (before ADR-1444) | not recorded |
| `float_vif_hip`, gfx1036 | 3840x2160 | 147 ms | 86 ms | 46 ms on 16 threads |

On an integrated GPU `float_vif_hip` is slower than the CPU. Most of the 4K
increase is the per-pixel term plane the row sums need
(`T-HIP-FLOAT-VIF-EXACT-THROUGHPUT-2026-10-02`).

## Cross-backend parity test

The `core/test/test_integer_vif_cpu_cuda_parity.c` smoke test (suite `fast`,
runs when `enable_cuda=true`) asserts CPU vs CUDA `vif_scaleN_score` agreement
within `1e-5` on the Netflix `src01_hrc00_576x324` reference pair, making the
luma-only parity claim a regression gate. The per-backend
`test_{cuda,sycl,hip}_float_vif_parity` tests each carry a variant that pins the
NEG option set and asserts parity on the derived `vif_scale0_egl_1_snsq_1.5`
key, so the default-options run cannot hide a kernel that ignores its options.

## Developer notes

### Floating-point stage dumps

A build compiled with `-DVIF_OPT_DEBUG_DUMP` writes intermediate floating-point
VIF data into an existing `stage/` directory relative to the process working
directory. Create that directory in a disposable run workspace before invoking
the binary. This compile-time diagnostic is separate from the extractor's
`debug=true` score-output option.

- For each computed scale `N`, `ref[N].bin` and `dis[N].bin` contain the
  current scale's source planes; `mu1`, `mu2`, `ref_sq_filt`, `dis_sq_filt` and
  `ref_dis_filt` contain filtered planes.
- Files contain row-major native-endian 32-bit floats without row padding or a
  header. Scale dimensions follow the pyramid above. When border handling is
  disabled at build time, filtered dumps exclude the filter's half-width
  border.
- `num_array[N].bin` and `den_array[N].bin` each contain **one float**, the
  scale's reduced numerator and denominator. They are not image-sized maps.
- Dump I/O does not alter metric scores; open, short-write and close failures
  are logged. A missing directory prevents file output. Use the normal
  production build for timing measurements.

The old optional path referenced an absent writer and null output pointers; the
checked writer now emits only initialized samples.

### CPU convolution boundary regression

Floating-point VIF shares separable convolution helpers with other CPU
features. The AVX2 and AVX-512 horizontal helpers now avoid loading discarded
lanes beyond a row, including tight final rows and planes narrower than the
filter radius. This is a memory-safety repair: each ISA retains its existing
per-pixel arithmetic and final scalar region. No score option or tolerance
changes are needed.

Developers can run this on an x86 assembly-enabled build:

```bash
python3 "$(git rev-parse --show-toplevel)/scripts/ci/run_meson_test.py" -- \
  -C build test_convolution_horizontal
```

The test checks normal, squared and cross-product filters; ASan/UBSan builds
also detect invalid reads that leave final scores unchanged. Unsupported CPU
ISAs are skipped, and disabled AVX-512 is omitted. See the
[boundary investigation](../research/convolution-horizontal-boundary-2026-09-08.md)
for the exact validation scope and negative controls.

## Former section names

ADRs and research digests link to these headings; each points to the section
that now holds its content.

### Developer-only floating-point stage dumps

Now under [floating-point stage dumps](#floating-point-stage-dumps).

## History

- **ADR-1166.** Before it, the `float_vif` size guard only covered scale 0 and
  admitted anything at or above 9x9, so frames in 9..15 px reached the scale-3
  convolution with a sub-minimum plane and read out of bounds
  ([Netflix/vmaf#1582](https://github.com/Netflix/vmaf/issues/1582)).
- **ADR-1166, what it means for old runs.** If you were scoring 9..15 px input,
  that run was reading uninitialised memory and its scores were not
  meaningful; upscale the input or use a smaller `vif_kernelscale`.
- **ADR-1444.** Before it, `float_vif_hip` ended with
  `Memory access fault by GPU node-1` on frames smaller than 72 pixels in either
  dimension (the CPU
  extractor accepts frames from 16x16).
- **ADR-1217.** GPU `float_vif` options reach the kernel (see the warning under
  [Options on the `float_vif` twins](#options-on-the-float_vif-twins)).
- **ADR-1412, ADR-1422, ADR-1444, ADR-1432, ADR-1435, ADR-1462.** The GPU twins
  became bit-exact; see the table under [GPU twins](#gpu-twins) for the earlier
  differences.

## See also

- [Features](features.md) - full feature extractor reference
- [ADR-0597](../adr/0597-integer-vif-luma-only-clarification.md) - why
  `enable_chroma` is a documented no-op on the CUDA twin.
- [ADR-1836](../adr/1836-cuda-vif-enable-chroma-names.md) - why its scores
  carry the `_enable_chroma` suffix when the option is set.
