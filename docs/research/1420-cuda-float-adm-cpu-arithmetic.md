<!-- markdownlint-disable MD013 MD060 -->
# Research-1420: Why float_adm_cuda was not the CPU's float_adm — nine causes, their sizes, and a division that belongs to the host processor

- **Status**: Active
- **Workstream**: [ADR-1420](../adr/1420-cuda-float-adm-cpu-arithmetic.md), [ADR-1403](../adr/1403-cuda-strict-fp-every-kernel.md), [ADR-0214](../adr/0214-gpu-parity-ci-gate.md)
- **Last updated**: 2026-10-01

## Question

With FMA contraction off ([ADR-1403](../adr/1403-cuda-strict-fp-every-kernel.md))
`float_adm_cuda` was still up to 1.3e-5 from `--backend cpu`. Which
operations differ, how much does each contribute, can the twin be made
identical, and what does that cost?

## Sources

- CPU: `core/src/feature/float_adm.c` (`extract()`), `core/src/feature/adm.c`
  (`compute_adm()`), `core/src/feature/adm_tools.c` (`rcp_s()`,
  `adm_decouple_s()`, `adm_csf_s()`, `adm_csf_den_scale_s()`, `adm_cm_s()`,
  `adm_cm_thresh3x3_s()`, `adm_dwt2_s()`), `core/src/feature/adm_tools.h`
  (`dwt_quant_step()`), `core/src/feature/adm_options.h`. No SIMD path is
  dispatched for float ADM on x86: `--cpumask 0` and `--cpumask 4294967295`
  give the same scores.
- CUDA: `core/src/feature/cuda/float_adm_cuda.c` and
  `float_adm/float_adm_score.cu` at master `5c8b9e9c7` (before) and on
  `fix/cuda-float-adm-cpu-arithmetic` (after).
- Reference workstation: RTX 4090 (sm_89, driver 615.71.09), CUDA 13.4 (`nvcc`
  V13.4.92), gcc 16.2.1, clang 22.1.8, glibc 2.44, Ryzen 9 9950X3D. `meson
  setup build-cuda core -Denable_cuda=true -Denable_sycl=false
  --buildtype=release -Db_lto=false`.
- Fixtures, 4:2:0, `--precision max`: the Netflix pair
  `src01_hrc00/01_576x324` at 8 bits (48 frames) and its 10-, 12- and 16-bit
  versions (3 frames each), the checkerboard pairs
  `checkerboard_1920_1080_10_3_0_0` against `_1_0` and `_10_0` (3 frames
  each), and the first 50 frames of BBB 3840x2160: 113 frames, seven scores
  each (`adm2`, `adm_scale0..3`, `aim`, `adm3`), 791 scores; with
  `debug=true` 18 outputs each, 2 034.

## Findings

### 1. Before

| Fixture | Scores identical | Largest difference | With `debug=true` |
|---|---:|---:|---:|
| Netflix 576x324, 8 bit | 66 / 336 | 2.5e-6 (`adm_scale1`) | 302 / 864 |
| Checkerboard 1 px | 2 / 21 | 3.5e-7 (`adm_scale1`) | 9 / 54 |
| Checkerboard 10 px | 4 / 21 | 1.5e-7 (`adm_scale0`) | 23 / 54 |
| BBB 3840x2160 | 66 / 350 | 1.3e-5 (`adm_scale0`) | 282 / 900 |
| Netflix 10, 12, 16 bit (each) | 2 / 21 | 1.9e-7 (`adm_scale1`) | 14 / 54 |
| Total | 144 / 791 | 1.3e-5 | 658 / 2 034 |

The debug sums are fp32 values in the hundreds and thousands; their largest
difference was 1.5e-3 (`adm_num` on BBB).

### 2. The CPU's arithmetic, from source

What a twin has to do to return `--backend cpu`'s bits:

- **Input.** `picture_copy()` with offset -128; a high-bit-depth sample is
  divided by 4, 16 or 256 first.
- **DWT.** Four taps, vertical pass then horizontal, each tap one fp32 product
  and one fp32 add into an accumulator that starts at 0, without contraction
  (the functions carry `optimize("-ffp-contract=off")`). Out-of-range indices
  mirror as `-i` and `2n - i - 1`. The old kernels already did this.
- **Decouple** (`adm_decouple_s()`). The angle test compares
  `ot_dp * ot_dp >= cos_1deg_sq * o_mag_sq * t_mag_sq`: left to right, so
  `(cos^2 * |o|^2) * |t|^2`. `k = DIVS(t, o + eps)`, clamped with two
  ternaries, `rst = k * o`, and under the angle flag
  `rst = MIN(rst * adm_enhn_gain_limit, t)` for a positive `rst` and `MAX` for
  a negative one. `adm_enhn_gain_limit` is a `double`, so the product and the
  comparison are fp64 and the result is rounded to fp32 once.
- **`DIVS`.** With `__SSE2__` and `ADM_OPT_RECIP_DIVISION`,
  `DIVS(n, d) = n * rcp_s(d)` and
  `rcp_s(x) = xi + xi * (1.0f - x * xi)` with `xi = _mm_rcp_ss(x)`. Without
  the macro (MSVC does not define `__SSE2__`; ARM) it is `n / d`.
- **CSF** (`adm_csf_s()`). `dst = rfactor * src` in fp32 and
  `flt = FLOAT_ONE_BY_30 * fabsf(dst)`. `FLOAT_ONE_BY_30` is `0.0333333351`,
  a double literal: the product is fp64 and rounded once.
- **Weights.** `rfactor = 1.0f / dwt_quant_step(...)`, and `dwt_quant_step()`
  in `adm_tools.h` keeps `r`, the logarithm and `Q` in `double`.
- **Masking threshold** (`adm_cm_thresh3x3_s()`). Per band a `float sum`: the
  three filtered samples of the row above, the left one, then
  `sum += FLOAT_ONE_BY_15 * fabsf(src)` (an fp64 addend, the sum rounded
  once), the right one, the three of the row below. The three band sums are
  added into a second `float`. The sample before the first mirrors to index
  1, the sample past the last clamps.
- **Reductions.** `adm_csf_den_scale_s()` adds `|rfactor * ref|^3` and
  `adm_cm_s()` adds `max(|x| - thr, 0)^3`, the cube as `(x * x) * x` in fp32.
  Each adds a row into `float inner[3]` and folds it into `float accum[3]`.
  The result is `powf(accum, 1/p) + powf(area * noise_weight, 1/p)` per band,
  the three added in order. The AIM numerator is the same reduction over the
  additive signal with the threshold of the restored one and no noise floor.
- **Frame.** `num`, `den`, `aim_num` and `aim_den` are `double` sums of the
  per-scale floats; `num` and `den` are zeroed below
  `1e-10 * (w * h) / (1920 * 1080)`.

### 3. The reciprocal estimate of this processor

`RCPSS` is specified by its error (at most 1.5 * 2^-12 relative), and the SDM
leaves part of its range to the implementation. Measured on the Ryzen 9
9950X3D with `_mm_rcp_ss`:

- The estimate of a normal `x` depends on the sign, the exponent and the top
  12 mantissa bits only: over all 2^23 mantissas of `[1, 2)` the result equals
  the result of the first mantissa of its 4096-entry bucket. Twelve bits of
  the result's mantissa are used (`0x7ff800`).
- Scaling by the exponent is exact: `rcp(m * 2^e)` is `rcp(m) * 2^-e` for
  every exponent from 1 to 252 (16.6 million inputs checked, both signs).
- `rcp(+-0)` and `rcp(denormal)` are infinities, `rcp(+-inf)` zeros, and the
  two largest exponents (253, 254), whose result would be below the normal
  range, give a zero.
- `rcp_s(x)` is not the fp32 reciprocal `1 / x` for 2 562 503 of the 8 388 608
  mantissas.

So a 4096-entry table of this processor's estimates for `[1, 2)` plus integer
exponent arithmetic reproduces the instruction on the whole fp32 range.
`adm_reciprocal_model_probe()` builds that table at extractor start and checks
the model against the instruction: every mantissa at exponent 127, then every
sign and exponent (zeros, denormals, infinities and NaNs included) at 2 048
mantissas each, 9.4 million calls, 9.4 ms. A host on which the check fails
(another table width, an emulator) is given the IEEE reciprocal with the same
Newton step, which is exact when the host's estimate is the IEEE reciprocal
and is logged as not exact otherwise. No second processor was available to
measure.

### 4. The causes, one at a time

The exact twin was built first. Each row below puts one old construct back
into it and measures the 791 scores against `--backend cpu`.

| Old construct | Scores that differ | Largest |
|---|---:|---:|
| `cos_1deg_sq * (o_mag_sq * t_mag_sq)` in the angle test | 19 | 1.3e-5 (BBB `adm_scale0`) |
| Row reduced in 256 strided partial sums and a warp tree, rows added in fp64 | 533 | 4.4e-7 (1 px checkerboard `adm_scale1`) |
| Host copy of `dwt_quant_step()` with fp32 `r` and logarithm | 527 | 2.1e-7 (Netflix `adm_scale1`) |
| `t / (o + eps)` | 147 | 1.3e-7 (Netflix `adm_scale2`) |
| Threshold: 24 neighbours, then the three centres, one accumulator | 131 | 9.4e-8 (BBB `adm_scale3`) |
| fp32 `FLOAT_ONE_BY_15` | 39 | 7.2e-8 (Netflix `adm_scale2`) |
| fp32 `FLOAT_ONE_BY_30` | 2 | 1.5e-10 (Netflix `aim`) |
| fp32 gain limit | 0 | 0 at the default 100; 1.0e-7 at `adm_enhn_gain_limit=1.2` |
| `fminf` / `fmaxf` instead of the ternaries | 0 | only the sign of a zero in the CSF buffers |
| `cos^2` as the literal `0.99969541789740297f` | 0 | the same fp32 value |
| `1e-2` floor of the frame sums | 0 on the fixtures | `adm2` 1 instead of 0 on the isolated-sample case below |

The angle test is the largest by two orders of magnitude and the rarest: the
two associations differ in the last bit of the threshold, which matters only
for a sample whose reference and distorted vectors are about one degree
apart, but there the flag decides between `k * o` and the gain-limited value.

Four of the eight default weights were off: scale 0 by +1 and -2 units in the
last place (h/v and d), scale 1 h/v by +1, scale 2 d by -3. Over five viewing
distances and five display heights 135 of 200 weights differ.

All of the above put back together give the old twin's output bit for bit:
1 980 of 1 980 outputs on the Netflix pair at 8, 10 and 16 bits, both
checkerboards and 50 BBB frames. No other difference is involved.

The floor: a flat 576x324 16-bit frame whose reference has one sample one
level up, scored with `adm_noise_weight=0`, has `adm_den = 2.8e-4`. The CPU's
floor is 9e-12 there and the old twin's 9e-4, so the twin zeroed the
denominator and reported `adm2 = 1` where the CPU reports 0.

### 5. After

Every output of every frame equals `--backend cpu`: 791 of 791 scores and
2 034 of 2 034 outputs with `debug=true`, also when clang's CUDA driver builds
the kernels (`-Denable_nvcc=false`). The parity gate reports 0 on all 200 BBB
frames and on the Netflix pair. Identical as well, each on the Netflix pair,
both checkerboards, BBB and the 10-bit Netflix pair (1 819 outputs):
`adm_enhn_gain_limit` 1.2 and 1, `adm_bypass_cm=1`, `adm_noise_weight=0`,
`adm_skip_aim_scale=2`, `adm_norm_view_dist=1.5` with
`adm_ref_display_height=2160`, `adm_csf_scale=2` with
`adm_csf_diag_scale=0.5`, and `adm_adm3_apply_hm` with `adm_dlm_weight=0.3`
and `adm_min_val=0.2`.

Frames from 17x17 up are identical (17x17, 18x34, 33x17, 32x32, 34x34, 63x65,
322x182 checked). Below that the CPU extractor is not a reference: at 16x16
its coarsest bands have one sample and `adm_cm_thresh3x3_s()` reads index 1,
a sample the previous scale left in the buffer; at 8x8 the scale-3 DWT input
has one sample and `dwt2_src_indices_filt_s()` yields index -1 for the fourth
tap (read from the index arithmetic; no sanitizer run). The CPU reports
`adm_scale3 = 1.047` for a random 8x8 pair.

### 6. What is not exact

`adm_p_norm` other than 3 replaces the cube by `powf(x, p)`, glibc's on the
CPU and CUDA's on the device.

| `adm_p_norm` | Scores that differ | Largest |
|---|---:|---:|
| 1 | 0 | 0 |
| 2 | 72 | 9.9e-8 (BBB `adm_scale3`) |
| 3 (default) | 0 | 0 |
| 4.5 | 58 | 9.6e-8 (BBB `adm_scale2`) |
| 20 | 20 | 1.1e-7 (Netflix `adm_scale0`) |

### 7. Cost

Against master `5c8b9e9c7`, host load average 6:

- A run of the twin alone, 3840x2160, `(t(52) - t(2)) / 50`, seven
  alternating pairs: 1.87 ms per frame before, 1.98 ms after; paired
  difference +0.12 ms, quartiles -0.06 to +0.26, after slower in five of
  seven. At 576x324: 0.20 and 0.23 ms, paired +0.04 (-0.13 to +0.30).
- The kernels of one more instance in a process that already has the frame on
  the device (nine `float_adm_cuda` instances with nine `adm_noise_weight`
  values against one, 50 BBB frames, `(t9 - t1) / (8 * 50)`, five alternating
  runs): 0.76 ms per frame before (0.745 to 0.792), 1.11 ms after (1.021 to
  1.137).
- Memory: nine fp32 terms per sample of the reduced scale-0 region,
  9 x 1538 x 866 x 4 bytes = 48 MB at 3840x2160.
- Extractor start: 9.4 ms for the probe.

Where the time goes was not measured. The new stages store and re-read 48 MB
of terms at scale 0 and add each row in one thread; the old ones reduced in
place.

### 8. Found on the way

- The CPU's float ADM depends on the host processor through `RCPSS`
  (section 3).
- The CPU extractor below 17x17 (section 5).
- `float_adm` files its debug ratio under the key `adm` whatever its options,
  because its `provided_features` lists `adm_scale0` where the emitted name is
  `adm`. Two instances with `debug=true` and different options fail with
  `feature "adm" cannot be overwritten`. `float_adm_cuda` lists `adm` and
  suffixes it.
- The device DWT mirrored a one-sample input to index 1 and -1, like the CPU.
  It now stays inside the buffer.

## Reproduce

```bash
meson setup build-cuda core -Denable_cuda=true -Denable_sycl=false \
    --buildtype=release -Db_lto=false && ninja -C build-cuda
# bit identity on the Netflix pair and 50 BBB 4K frames
python3 scripts/dev/speed_gpu_parity.py --backend cuda \
    --vmaf $PWD/build-cuda/tools/vmaf --feature float_adm
# the gate cell (tolerance 0)
python3 scripts/ci/cross_backend_parity_gate.py \
    --vmaf-binary build-cuda/tools/vmaf \
    --reference testdata/bbb/ref_3840x2160_200f.yuv \
    --distorted testdata/bbb/dis_3840x2160_200f.yuv \
    --width 3840 --height 2160 --features float_adm --backends cpu cuda
# host arithmetic, device cases, design
build-cuda/test/test_float_adm_device_math
build-cuda/test/test_cuda_float_adm_parity
python3 core/test/test_cuda_float_adm_exact_contract.py
```
