<!-- markdownlint-disable MD013 MD060 -->
# Research-1403: FMA contraction off for every CUDA kernel — which kernels change, what each twin does against the CPU, what it costs

- **Status**: Active
- **Workstream**: [ADR-1403](../adr/1403-cuda-strict-fp-every-kernel.md), [ADR-1367](../adr/1367-sycl-strict-fp-every-feature-tu.md), [ADR-0214](../adr/0214-gpu-parity-ci-gate.md)
- **Last updated**: 2026-10-01

## Question

`T-CUDA-FP-CONTRACT-DEFAULT-2026-09-29`: nvcc fuses `a * b + c` into one FMA
by default, and only six of the 21 CUDA fatbins were built with
`--fmad=false`. The CPU reference and, since ADR-1367, every SYCL twin do not
contract. If every CUDA kernel is built without contraction: which kernels'
code changes, which twins move against `--backend cpu`, does any twin rely on
a fused operation, and what does a 3840x2160 frame cost before and after?

## Sources

- `core/src/meson.build` (`cuda_cu_sources`, the fatbin `custom_target`),
  `core/src/feature/cuda/**/*.cu`, the CPU extractors they mirror.
- RTX 4090 (sm_89, driver 615.71.09), nvcc 13.4.92, gcc 16.2.1, glibc 2.44,
  Ryzen 9 9950X3D. `meson setup <dir> core -Denable_cuda=true
  -Denable_sycl=false --buildtype=release -Db_lto=false`. Before: `master`
  `bd061d95a`. After: the same plus this change. (Two earlier rounds measured
  the same numbers for every twin they did not concern: on `2c3acf1c9`,
  before ADR-1397 gave `psnr_hvs_score` the flag and its CPU-order sum, and
  on `bd17c352f`, before ADR-1399 rewrote `ssim_score.cu` with explicit
  rounding intrinsics. The `float_ms_ssim` steps in finding 3 and the fp64
  cost are from the first round, the timings in finding 5 from the second.)
- Fixtures, 8-bit 4:2:0, `--precision max`: the Netflix pair
  `src01_hrc00/01_576x324` (48 frames), the checkerboard pairs
  `checkerboard_1920_1080_10_3_0_0` against `_1_0` and `_10_0` (3 frames
  each), and the first 50 frames of BBB 3840x2160
  (`testdata/bbb/ref/dis_3840x2160_200f.yuv`).
- Every twin is requested by its registered name (`--backend cuda --feature
  <name>_cuda`), the CPU extractor with `--backend cpu --threads 16`.

## Findings

### 1. Fifteen fatbins keep their machine code, six change

`cuobjdump -sass` of every fatbin, before against after, for each shipped
architecture (sm_80, sm_86, sm_89, sm_90, sm_100, sm_120); FMA counts are the
sm_89 cubin's.

| Fatbin | Twin | Machine code | fp32 FMA | fp64 FMA |
|---|---|---|---:|---:|
| `speed_score`, `ssimulacra2_blur`, `ssimulacra2_device`, `float_adm_score`, `integer_ssim_score`, `psnr_hvs_score` | `speed_*`, `ssimulacra2`, `float_adm`, `ssim`, `psnr_hvs` | already `--fmad=false`; fatbins byte-identical | unchanged | unchanged |
| `adm_dwt2`, `adm_cm`, `adm_csf`, `adm_csf_den` | `adm` | identical on all six | 0 / 746 / 52 / 0 | 0 |
| `cambi_score`, `moment_score`, `motion_v2_score`, `psnr_score` | `cambi`, `float_moment`, `motion`, `motion_v2`, `psnr` | identical on all six | 0 | 0 |
| `ssim_score` | `float_ssim` | identical on all six: every rounding is an explicit intrinsic (ADR-1399) | 110 | 48 |
| `filter1d` | `vif` | changes | 370 -> 370 | 130 -> 120 |
| `float_psnr_score` | `float_psnr` | changes | 10 -> 9 | 0 |
| `float_motion_score` | `float_motion` | changes | 77 -> 9 | 0 |
| `float_vif_score` | `float_vif` | changes | 330 -> 123 | 0 |
| `ciede_score` | `ciede` | changes | 938 -> 834 | 0 |
| `ms_ssim_score` | `float_ms_ssim` | changes | 131 -> 8 (flag only) | 34 -> 31 (flag only) |

For the nine identical ones the only difference in the fatbin is the PTX
text: `mul.f32` / `mul.f64` become `mul.rn.f32` / `mul.rn.f64` and the header
gains `ptxasOptions = --fmad false`. nvcc had formed no FMA from their source:
the FMAs counted in `adm_cm`, `adm_csf` and `ssim_score` are in the PTX of
neither build, they are the back end's own expansion of division and square
root. The FMAs
that remain in the changed kernels after the flag are of that kind
(division, square root, libdevice math functions), plus the explicit ones.

### 2. Per-twin agreement with the CPU, before and after

Identical frame outputs / all frame outputs (every output of the twin, every
frame), then the largest and the mean absolute difference over them. "Same"
means the twin's own value is unchanged on every frame.

| Twin | Fixture | Identical before | Identical after | Max before | Max after | Mean before | Mean after |
|---|---|---:|---:|---:|---:|---:|---:|
| `vif`, `motion`, `motion_v2`, `psnr`, `psnr_hvs`, `float_psnr`, `float_moment`, `float_ssim`, `cambi`, `speed_temporal` | all four | all | all | 0 | 0 | 0 | 0 |
| `float_ms_ssim` | Netflix | 0/48 | 48/48 | 6.89e-8 | 0 | 2.45e-8 | 0 |
| | checkerboard 1 px | 0/3 | 3/3 | 2.14e-6 | 0 | 1.47e-6 | 0 |
| | checkerboard 10 px | 0/3 | 3/3 | 4.42e-6 | 0 | 3.76e-6 | 0 |
| | BBB 4K | 0/50 | 50/50 | 5.82e-7 | 0 | 2.49e-7 | 0 |
| `ciede` | Netflix | 0/48 | 0/48 | 1.14e-5 | 1.14e-5 | 1.02e-5 | 1.03e-5 |
| | checkerboards | 0/6 | 0/6 | 8.55e-7 | same | 8.55e-7 | same |
| | BBB 4K | 0/50 | 0/50 | 1.40e-6 | 1.49e-6 | 1.17e-6 | 7.70e-7 |
| `float_motion` | Netflix | 2/144 | 2/144 | 3.01e-6 | 3.12e-6 | 9.11e-7 | 9.18e-7 |
| | checkerboards | 4/18 | 4/18 | 1.35e-4 | 1.36e-4 | 9.69e-5 | 9.76e-5 |
| | BBB 4K | 2/150 | 2/150 | 2.37e-5 | 2.36e-5 | 8.66e-7 | 8.64e-7 |
| `float_vif` | Netflix | 0/192 | 0/192 | 2.72e-5 | 3.81e-5 | 3.07e-6 | 3.18e-6 |
| | checkerboard 1 px | 0/12 | 0/12 | 1.02e-6 | 1.04e-6 | 4.44e-7 | 4.72e-7 |
| | checkerboard 10 px | 10/12 | 10/12 | 1.12e-12 | 1.14e-12 | 1.04e-13 | 1.05e-13 |
| | BBB 4K | 0/200 | 0/200 | 5.75e-6 | 7.03e-6 | 1.94e-6 | 1.83e-6 |
| `adm` | all four | 176/728 | same | 2.09e-7 | same | 2.8e-8 | same |
| `float_adm` | all four | 138/728 | same | 1.28e-5 | same | 7.2e-8 | same |
| `ssim` (integer) | all four | 0/104 | same | 1.06e-11 | same | 5.1e-13 | same |
| `ssimulacra2` | all four | 8/104 | same | 7.28e-11 | same | 2.0e-12 | same |
| `speed_chroma` | all four | 306/312 | same | 1.43e-6 | same | 1.9e-8 | same |

`float_ssim_cuda` runs at every scale since ADR-1399 and was bit-identical
before this change; on the previous base (`bd17c352f`, scale 1 only, plain
fp32 window sums) the flag moved it from 1.79e-7 to 1.19e-7 on the Netflix
pair. `speed_chroma`'s six differing outputs are the CPU's glibc `log2f`
(ADR-1380), not the twin.

Against the ADR-0214 tolerances (`FEATURE_TOLERANCE` in
`scripts/ci/cross_backend_parity_gate.py`; the gate runs the Netflix pair):
every twin is inside its tolerance on the Netflix pair before and after.
Run as the gate itself (`--backends cpu cuda`): all 18 cells pass before and
after (`adm`, `cambi`, `ciede`, `float_adm`, `float_moment`, `float_motion`,
`float_ms_ssim`, `float_ms_ssim_lcs`, `float_psnr`, `float_ssim`,
`float_ssim_lcs`, `float_vif`, `motion`, `motion_v2`, `psnr`, `psnr_hvs`,
`ssimulacra2`, `vif`). One
is outside it on larger frames, before and after alike, for a reason the flag
does not touch: `float_motion` on the checkerboards (1.36e-4 against 5e-5;
finding 4).

### 3. `float_ms_ssim_cuda` relied on contraction in two ways; with the reference's arithmetic it is bit-identical

With the flag alone the twin gets closer on small frames and further at 4K.
The steps, each on top of the previous (max / mean absolute difference):

| Step | Netflix | Checkerboard 1 px | Checkerboard 10 px | BBB 4K |
|---|---|---|---|---|
| before | 6.89e-8 / 2.45e-8 | 2.14e-6 / 1.47e-6 | 4.42e-6 / 3.76e-6 | 5.82e-7 / 2.49e-7 |
| `--fmad=false` only | 5.53e-8 / 1.97e-8 | 1.05e-6 / 5.38e-7 | 2.97e-6 / 2.26e-6 | 1.23e-6 / 7.57e-7 |
| + decimate taps as `__fmaf_rn()` | 6.80e-8 / 2.26e-8 | 1.57e-6 / 8.49e-7 | 3.16e-6 / 2.47e-6 | 1.13e-6 / 7.24e-7 |
| + window sums in fp64 | 2.73e-8 / 8.28e-9 | 3.19e-8 / 1.65e-8 | 3.45e-8 / 2.25e-8 | 3.49e-8 / 1.07e-8 |
| + reference operand types, fp32 means | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| window sums as an fp32 pair instead of fp64 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |

What differed from the CPU, in the order the steps remove it:

1. `ms_ssim_decimate.c` accumulates each low-pass tap with one fused
   multiply-add (`vmaf_fmaf_exact()`, `fmadd` in the SIMD twins). The kernel
   wrote `acc += sample * tap` and matched only because nvcc fused it.
2. `iqa_convolve()` forms each Gaussian-window product in fp32 and adds the
   products in fp64, then rounds to fp32 once per pass. The kernel kept an
   fp32 running sum. Fusing each step, which keeps the product exact, hid
   most of the difference; without fusion it shows, most at 4K.
3. `ssim_accumulate_default_scalar()` divides an fp64 numerator by an fp32
   denominator for `l` and `c`, and computes `s` as an fp32 quotient; its
   constants `C1`, `C2`, `C3` are fp32. The kernel promoted all of it to fp64.
4. `iqa_ssim()` returns each per-scale mean as a float and `ms_ssim.c` raises
   the floats to the Wang exponents. The host kept fp64 means, so the 15
   `enable_lcs` outputs were up to 1.3e-6 off as well.

The fp64 window sums cost 3.4 ms per 3840x2160 frame: 10.73 -> 14.09 ms
(7 alternating repetitions of 100 frames; 10.49 -> 14.44 with 5 repetitions
of 20 frames). An fp32 pair (running sum plus the exact error of each
addition, six fp32 operations per term) carries the 11-term sum to about 48
bits, gives the same 104 frame scores, and costs nothing measurable
(10.54 -> 10.57, 7 repetitions of 100 frames). It is what ships. It is not the reference's operation literally: the pair and the 53-bit
sum can round to different fp32 values when the exact sum lies within about
2^-18 ulp of a rounding boundary, which then moves one pixel's statistic by
one ulp. The fp64 `l` / `c` / `s` block sums differ from the CPU's raster
order by more than that already; the fp32 rounding of the mean absorbs both.

### 4. What the remaining differences are

- `float_motion`: the CPU keeps one fp32 running sum of the absolute
  differences of a whole plane (`float_sad_line_c()` per row,
  `compute_motion_simd()` over the rows) and divides in fp32. The kernel sums
  per 16x16 block and the host adds the blocks in fp64. Replayed on the host
  for the 1920x1080 checkerboard: blur and differences in fp32 with separate
  multiply and add, then the fp32 running sum, reproduces the CPU's score bit
  for bit (18.805618286132812 and 18.8588924407959 for frames 1 and 2); the
  fp64 sum of the same differences is 1.17e-4 and 1.35e-4 lower, and the twin
  is within 6e-7 of that. So the blur matches and the whole gap is the CPU's
  own rounding. `T-CUDA-FLOAT-MOTION-CPU-FLOAT-SUM-2026-10-01`.
- `float_vif`: not isolated. The kernel calls the device `log2f` where the
  CPU calls glibc's on an fp64-promoted argument, and it reduces per block.
  The SYCL twin shows the same two numbers on the Netflix pair before and
  after ADR-1367 (2.71e-5 and 3.81e-5), so contraction-free CUDA and SYCL
  now agree with each other. `T-CUDA-FLOAT-VIF-RESIDUAL-2026-10-01`.
- `ciede`: per-pixel `pow`, `sqrt`, `sin`, `atan2` on the device against
  libm; tolerance 5e-3.

### 5. Cost

3840x2160, BBB, one twin per run, `(t(N) - t(2)) / (N - 2)` ms per frame,
alternating pairs with the order swapped every pair; median before and after,
then the median and the quartiles of the paired difference. The host is
shared; twins whose machine code is identical show what it resolves.

15 pairs, N = 102, load average 9 to 16:

| Twin | Code | Before | After | Paired difference | After slower in |
|---|---|---:|---:|---|---:|
| `vif` | changed | 3.42 | 3.44 | -0.07 (-0.30 .. +0.21) | 5/15 |
| `float_psnr` | changed | 2.62 | 2.61 | -0.07 (-1.03 .. +0.13) | 4/15 |
| `float_motion` | changed | 2.73 | 3.03 | +0.21 (-0.24 .. +0.44) | 10/15 |
| `float_vif` | changed | 2.53 | 2.65 | +0.12 (-0.02 .. +0.22) | 11/15 |
| `ciede` | changed | 2.42 | 2.46 | +0.04 (-0.05 .. +0.59) | 11/15 |
| `float_ms_ssim` | changed | 11.26 | 10.87 | -0.12 (-1.34 .. +0.62) | 6/15 |
| `psnr_hvs` | identical | 12.18 | 12.09 | -0.08 (-0.16 .. +0.10) | 7/15 |
| `psnr` | identical | 2.84 | 3.12 | +0.07 (-0.12 .. +0.30) | 11/15 |
| `adm` | identical | 3.50 | 3.47 | +0.04 (-0.11 .. +0.14) | 8/15 |
| `speed_chroma` | identical | 2.38 | 2.44 | +0.04 (-0.08 .. +0.16) | 8/15 |

The two largest, repeated with 21 pairs and N = 200, load average 9:

| Twin | Code | Before | After | Paired difference | After slower in |
|---|---|---:|---:|---|---:|
| `float_motion` | changed | 2.82 | 2.89 | +0.10 (-0.22 .. +0.27) | 14/21 |
| `float_vif` | changed | 2.43 | 2.56 | 0.00 (-0.04 .. +0.05) | 12/21 |
| `psnr` | identical | 2.61 | 2.38 | -0.06 (-0.35 .. +0.07) | 8/21 |
| `float_moment` | identical | 2.53 | 2.45 | -0.03 (-0.11 .. +0.24) | 10/21 |

No paired difference is distinguishable from the controls; the other
thirteen twins run the same code as before. At 4K these twins spend their time reading
and uploading the frame, not in the kernels (what the flag adds to
`float_motion`, about 70 separate multiplies and adds per pixel, is a few
microseconds per 4K frame on this GPU). A kernel-only measurement needs a
quiet host or an in-process harness; `vmaf_bench --gpu-profile` is SYCL-only.

### 6. The clang CUDA path

`-Denable_nvcc=false` did not configure on `master`: the shared fatbin
command concatenates `nvcc_ccbin_flags` and `nvcc_host_includes`, which only
the nvcc branch assigned. With both assigned (empty) and
`-ffp-contract=off`, clang 22.1.8 builds all 21 kernels against CUDA 13.4
(it warns that the SDK is newer than it knows). Its PTX has `mul.rn.f32` /
`add.rn.f32` throughout and `fma.rn.f32` only where the source asks for it.

Run on the RTX 4090 against the nvcc build of the same tree, on the four
fixtures (BBB: 22 frames): 18 of the 19 twins give the nvcc build's value on
every output of every frame (3 952 outputs), among them the ones that are not
bit-identical to the CPU (`adm`, `float_adm`, `float_vif`, `float_motion`,
`ssim`, `ssimulacra2`, `speed_chroma`); `float_ssim` and `float_ms_ssim` are
bit-identical to the CPU under both compilers. The one that differs
calls device math functions: `ciede` (6 of 76 outputs equal, 1.13e-5 from the
CPU against nvcc's 1.14e-5). The first
clang build of `float_ms_ssim_cuda` matched the CPU on 9 of 48 frames
(3.4e-8): clang compiles `sqrtf()` to `sqrt.approx.f32` where nvcc emits
`sqrt.rn.f32`. With `__fsqrt_rn()` the clang build is bit-identical to the
CPU on all 104 frames as well, and the nvcc machine code is unchanged.

## Reproduce

```sh
meson setup build-cuda core -Denable_cuda=true -Denable_sycl=false --buildtype=release -Db_lto=false
ninja -C build-cuda
# which fatbins hold FMAs
for f in build-cuda/src/*.fatbin; do printf '%s %s\n' "$f" "$(cuobjdump -sass -arch sm_89 "$f" | grep -c FFMA)"; done
# one twin against the CPU
python3 scripts/dev/speed_gpu_parity.py --backend cuda --vmaf "$PWD/build-cuda/tools/vmaf" \
  --netflix-dir python/test/resource/yuv --bbb-dir testdata/bbb --feature float_ms_ssim
build-cuda/test/test_cuda_float_ms_ssim_parity
python3 -m pytest core/test/test_strict_fp_compiler_args.py core/test/test_cuda_kernel_source_contract.py
```
