<!-- markdownlint-disable MD013 MD060 -->
# Research-1412: Why float_vif_cuda was not the CPU's float_vif — four causes, their sizes, and the cost of removing them

- **Status**: Active
- **Workstream**: [ADR-1412](../adr/1412-cuda-float-vif-cpu-arithmetic.md), [ADR-1403](../adr/1403-cuda-strict-fp-every-kernel.md), [ADR-0214](../adr/0214-gpu-parity-ci-gate.md)
- **Last updated**: 2026-10-01

## Question

`T-CUDA-FLOAT-VIF-RESIDUAL-2026-10-01`: with FMA contraction off
(ADR-1403), `float_vif_cuda` still matched `--backend cpu` on no frame and was
3.8e-5 away on the Netflix pair. The row suspected the device `log2f` and the
order of the sums. Which operations actually differ, how much does each
contribute, can the twin be made identical, and what does that cost?

## Sources

- CPU: `core/src/feature/float_vif.c` (`init()`, `extract()`),
  `core/src/feature/vif.c` (`compute_vif()`), `core/src/feature/vif_tools.c`
  (`vif_get_filter()`, `vif_filter1d_*_s()`, `vif_statistic_s()`,
  `log2f_approx()`), `core/src/feature/vif_options.h`,
  `core/src/feature/common/convolution_avx.c` and `convolution_internal.h`
  (the path this host runs).
- CUDA: `core/src/feature/cuda/float_vif_cuda.c` and
  `float_vif/float_vif_score.cu` at master `24ac5bbb3` (before; the same
  kernel at `95df9adbe`, the base the timings use) and on
  `fix/cuda-float-vif-cpu-arithmetic` (after).
- Host `zeus`: RTX 4090 (sm_89, driver 615.71.09), CUDA 13.4 (`nvcc`
  V13.4.92), gcc 16.2.1, clang 22.1.8, glibc 2.44, Ryzen 9 9950X3D. `meson
  setup build-cuda core -Denable_cuda=true -Denable_sycl=false
  --buildtype=release -Db_lto=false`.
- Fixtures, 4:2:0, `--precision max`: the Netflix pair
  `src01_hrc00/01_576x324` at 8 bits (48 frames) and its 10-, 12- and 16-bit
  versions (3 frames each), the checkerboard pairs
  `checkerboard_1920_1080_10_3_0_0` against `_1_0` and `_10_0` (3 frames
  each), and the first 50 frames of BBB 3840x2160.

## Findings

### 1. The CPU's arithmetic, from source

A host program that reads the fixture and recomputes the four scales was
written until it gave `--backend cpu`'s `vif_scale0..3` bit for bit on every
frame of every fixture. What it has to do:

- **Input.** `picture_copy()` with offset -128; a high-bit-depth sample is
  divided by 4, 16 or 256 first.
- **Taps.** `float_vif.c::init()` calls `vif_get_filter()` per scale:
  `exp()` in fp64 rounded to fp32, an fp32 running sum, one fp32 division
  per tap. The widths are 17, 9, 5, 3.
- **Filters.** Vertical pass, then horizontal, each tap one fp32 product and
  one fp32 add, taps in order, reflect-101 at both edges. The AVX2 path
  (`convolution_f32_avx_*`) and its scalar edge helper give the same value:
  the build does not contract (`-std=c23` under gcc; the edge helper names
  its product).
- **Decimation.** The next scale is this scale's filter over the previous
  plane, sampled at even positions.
- **Statistic.** `vif_pixel_statistic_s()`. `vif_sigma_nsq` is a `double`
  parameter, so `sv_sq + vif_sigma_nsq`, the quotient and `1.0f + ...` are
  fp64; the result is rounded to fp32 when it is passed to `log2f`.
- **`log2f`.** `vif_options.h` defines `VIF_OPT_FAST_LOG2`, and under it
  `vif_tools.c` has `#define log2f log2f_approx`: exponent minus 127 plus an
  fp32 Horner polynomial of nine coefficients in `mantissa - 1`. libm is not
  called, so the result does not depend on the host's C library.
- **Sums.** `vif_statistic_s()`: one fp32 accumulator per row, left to right,
  added into a second fp32 accumulator top to bottom. `compute_vif()` widens
  the two floats to `double` and the collector divides them.

### 2. What the old kernel did differently, and how much each is worth

The replay was then changed one property at a time to what the kernel did,
and once to all four. Largest absolute difference from the CPU over the
frames of a fixture (BBB: 8 frames):

| Fixture | Output | Tap table | libm `log2f` | fp32 `vif_sigma_nsq` | Block order | All four | Old twin, measured |
|---|---|---:|---:|---:|---:|---:|---:|
| Netflix 576x324 | `vif_scale0` | 4.37e-6 | 4.85e-8 | 2.43e-8 | 3.62e-7 | 4.18e-6 | 4.18e-6 |
| | `vif_scale1` | 8.51e-6 | 1.30e-7 | 1.30e-7 | 4.29e-7 | 8.35e-6 | 8.35e-6 |
| | `vif_scale2` | 1.88e-5 | 7.10e-8 | 7.09e-8 | 5.15e-7 | 1.89e-5 | 1.89e-5 |
| | `vif_scale3` | 3.83e-5 | 2.15e-7 | 0 | 3.40e-7 | 3.81e-5 | 3.81e-5 |
| Checkerboard 1 px | `vif_scale0` | 9.71e-9 | 0 | 0 | 1.00e-6 | 1.01e-6 | 1.01e-6 |
| | `vif_scale1` | 3.72e-7 | 0 | 0 | 6.38e-7 | 5.98e-7 | 5.98e-7 |
| | `vif_scale2` | 1.21e-7 | 0 | 0 | 8.01e-7 | 8.26e-7 | 8.26e-7 |
| | `vif_scale3` | 5.12e-7 | 0 | 0 | 5.69e-7 | 1.04e-6 | 1.04e-6 |
| Checkerboard 10 px | `vif_scale0` | 1.85e-13 | 0 | 0 | 1.25e-12 | 1.14e-12 | 1.14e-12 |
| | `vif_scale1..3` | 0 | 0 | 0 | 0 | 0 | 0 |
| BBB 3840x2160 | `vif_scale0` | 2.54e-6 | 1.79e-9 | 3.19e-8 | 2.24e-6 | 2.25e-6 | 2.25e-6 |
| | `vif_scale1` | 7.01e-7 | 7.14e-8 | 0 | 2.73e-6 | 2.29e-6 | 2.29e-6 |
| | `vif_scale2` | 3.87e-6 | 1.20e-7 | 0 | 1.19e-6 | 3.79e-6 | 3.79e-6 |
| | `vif_scale3` | 2.07e-6 | 6.26e-8 | 7.49e-8 | 6.04e-7 | 1.71e-6 | 1.71e-6 |

"All four" replays the old kernel on the host with glibc's `log2f`; it is
within 1.4e-8 of the twin's own output on every row, which is the device
`log2f` against glibc's. So the four causes account for the whole distance.

- **Tap table.** The kernel carried `FVIF_COEFF_S0..S3`, the decimal values of
  the `vif_filter1d_table_s` the CPU used until #758
  ([ADR-0416](../adr/0416-vif-upstream-onthefly-filter-sync.md)) replaced it
  with the run-time `vif_get_filter()`. The difference to the computed taps,
  in units in the last place, tap by tap: scale 0
  `0 0 0 0 -2 -1 -1 -2 -14 -2 -1 -1 -2 0 0 0 0`, scale 1
  `-1 -1 1 1 -3 1 1 -1 -1`, scale 2 `-1 -1 -1 -1 -1`, scale 3 `1 -1 1`: 26 of
  34 taps. It is the dominant term wherever the picture has texture, and it
  grows with the scale because the decimated planes are filtered again.
- **Order.** The dominant term at 3840x2160 and on the 1 px checkerboard
  (long rows of near-equal terms).
- **`log2f`, types.** Each below 2.2e-7, and each alone still moves the
  score on the Netflix pair and at 3840x2160.

The row's two suspects were the second and third largest. The first was
found only because the replay had to reproduce the CPU before it was allowed
to explain the twin.

### 3. After: identical on every output

`--backend cpu --feature float_vif` against `--backend cuda --feature
float_vif_cuda`, identical outputs / all outputs, largest difference:

| Fixture | Before | After |
|---|---|---|
| Netflix 576x324, 8-bit, 48 frames | 0/192, 3.81e-5 | 192/192, 0 |
| Checkerboard 1 px, 3 frames | 0/12, 1.04e-6 | 12/12, 0 |
| Checkerboard 10 px, 3 frames | 10/12, 1.14e-12 | 12/12, 0 |
| BBB 3840x2160, 50 frames | 0/200, 7.03e-6 | 200/200, 0 |
| Netflix 576x324, 10-bit, 3 frames | 0/12, 1.07e-5 | 12/12, 0 |
| Netflix 576x324, 12-bit, 3 frames | 0/12, 1.07e-5 | 12/12, 0 |
| Netflix 576x324, 16-bit, 3 frames | not measured | 12/12, 0 |

With `debug=true` (15 outputs: the four ratios, the frame ratio, its numerator
and denominator and the eight per-scale sums) on the first five fixtures:
1 605 of 1 605. The parity gate over all 200 BBB frames and over the Netflix
pair reports `tol=0.0e+00 (exact:ADR-1397) max_abs_diff=0.000e+00`. Built
with clang's CUDA driver (`-Denable_nvcc=false`) the kernels give the same
1 605 of 1 605.

Each cause was checked against the shipped header by putting it back alone
(`float_vif_device.h` edited, `test_float_vif_device_math` rebuilt): an fp32
`vif_sigma_nsq`, a libm `log2f` and a pairwise row sum each fail the test.
The old twin fails `test_cuda_float_vif_parity` on its first case (up to
8.8e-6 on the 256x144 fixture).

### 4. Cost

3840x2160, BBB, RTX 4090, shared host. "Before" is master `95df9adbe`, the
base of this change (it includes the pinned CLI upload of ADR-1406).

- **One twin per run**, `(t(200) - t(2)) / 198` ms per frame, nine
  alternating pairs, load average 20 to 24: 1.97 before, 1.96 after, paired
  difference +0.02 (quartiles -0.37 to +0.11, after slower in 5 of 9). At
  this size the run waits for the frame to be read and uploaded, not for the
  kernels. On the earlier base `24ac5bbb3` (pageable upload, load 8 to 9):
  2.42 and 2.31, paired difference -0.09. `scripts/dev/speed_gpu_parity.py`
  on that base (median of 3 of `(t(22) - t(2)) / 20`, load 10) read 2.88 ms
  for the twin and 35.6 ms for 16 CPU threads.
- **The kernels alone.** One process with nine `float_vif_cuda` instances
  (nine values of `vif_enhn_gain_limit`) against one with a single instance,
  60 frames: `(t9 - t1) / (8 * 60)` is what one more instance costs per
  frame with the frame already on the device. Median of seven alternating
  runs:

  | Base | Load average | Before | After | fp32 statistic (not shipped) |
  |---|---|---:|---:|---:|
  | `95df9adbe` | 20 to 24 | 0.72 | 1.00 | not measured |
  | `24ac5bbb3` | 12 to 14 | 0.57 | 0.89 | 0.70 |

  So the twin's kernels cost about 0.3 ms more per frame. Of that, about
  0.19 ms is the fp64 arithmetic (two quotients and three sums per pixel,
  11 million pixels over the four scales) and the rest the term store plus
  the row-sum kernel.
- **Memory.** Two floats per pixel of scale 0: 66 MB at 3840x2160, next to
  50 MB of raw planes and scale buffers.

Candidates for the tuning pass
(`T-CUDA-FLOAT-VIF-EXACT-THROUGHPUT-2026-10-01`), none tried: the two fp64
quotients as exact fp32 pairs (also what an fp64-less SYCL twin needs); the
row-sum thread loading several terms per step, since its adds are serial but
its loads are not; skipping the scale-0 launches under `vif_skip_scale0`,
which the CPU skips and the twin still computes.

### 5. The other twins

`float_vif_sycl.cpp`, `hip/float_vif/float_vif_score.hip` and
`metal/float_vif.metal` hold the same tap literals and call `sycl::log2`,
`log2f` and `log2` (read from source). ADR-1403 measured the SYCL twin at the
same 3.81e-5 on the Netflix pair as the CUDA twin, which is the tap table.
`T-GPU-FLOAT-VIF-CPU-ARITHMETIC-2026-10-01`.

## Reproduce

```sh
meson setup build-cuda core -Denable_cuda=true -Denable_sycl=false --buildtype=release -Db_lto=false
ninja -C build-cuda
# Netflix 576x324 and BBB 3840x2160, every frame, with timing
python3 scripts/dev/speed_gpu_parity.py --backend cuda --vmaf "$PWD/build-cuda/tools/vmaf" \
  --netflix-dir python/test/resource/yuv --bbb-dir testdata/bbb --feature float_vif
# the header against vif_tools.c / vif.c on the host, the twin against the CPU on the device
build-cuda/test/test_float_vif_device_math
build-cuda/test/test_cuda_float_vif_parity
python3 core/test/test_cuda_float_vif_exact_contract.py
# the gate cell
python3 scripts/ci/cross_backend_parity_gate.py --vmaf-binary "$PWD/build-cuda/tools/vmaf" \
  --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
  --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
  --width 576 --height 324 --features float_vif --backends cpu cuda
```
