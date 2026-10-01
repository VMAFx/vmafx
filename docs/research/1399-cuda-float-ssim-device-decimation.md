<!-- markdownlint-disable MD013 MD060 -->
# Research-1399: float_ssim decimation and CPU-typed convolution on CUDA

- **Status**: Active
- **Workstream**: [ADR-1399](../adr/1399-cuda-float-ssim-device-decimation.md), [ADR-1370](../adr/1370-sycl-float-ssim-device-decimation.md), [ADR-1373](../adr/1373-cuda-twin-cpu-option-parity.md)
- **Last updated**: 2026-10-01

## Question

`float_ssim_cuda` computed scale 1 only, so `float_ssim` at 1920x1080 and 3840x2160 ran on the CPU under `--backend cuda` (`T-CUDA-FLOAT-SSIM-SCALE-GT1-2026-09-29`). Can the device reproduce the CPU's decimated planes byte for byte, how close is the score then, and what does it cost on an RTX 4090?

## Sources

- `core/src/feature/ssim.c` (`compute_ssim()`, `ssim_low_pass_alloc()`, `ssim_decimate_pair()`), `core/src/feature/iqa/decimate.c`, `core/src/feature/iqa/convolve.c` (`iqa_filter_pixel()`, `iqa_convolve_1d_separable()`), `core/src/feature/iqa/ssim_tools.c`, `core/src/feature/iqa/ssim_accumulate_lane.h`.
- `core/src/feature/sycl/integer_ssim_sycl.cpp` and [Research-2130](2130-sycl-float-ssim-device-decimation.md): the decimation design and its exactness proof.
- Host `ryzen-4090-arc`: RTX 4090, CUDA 13.4 (`nvcc` 13.4.92), release build without LTO, `--precision max`. The "before" build is master `430a39622`; the change was measured on top of `2c3acf1c9` (no `float_ssim` change between the two).
- Fixtures: the Netflix 576x324 pair at 8 bits (48 frames) and 10, 12 and 16 bits (3 frames each); the two 1920x1080 checkerboard pairs (3 frames each); BBB 3840x2160 8-bit (50 frames); a 1920x1080 pair at 8 and 10 bits scaled from BBB with ffmpeg (bicubic, 24 frames); BBB 3840x2160 at 10 bits (12 frames, the 8-bit samples shifted left by two with random low bits).

## Findings

### The decimated planes are the CPU's, byte for byte

`calculate_ssim_decimate_{8,16}bpc` forms the CPU's fp32 product per tap, adds the products in int64 units of 2^-52 and converts once with `__ll2float_rn()`. `core/test/test_cuda_float_ssim_decimate.c` launches the kernel on hashed full-range luma, reads both planes back and compares them with `iqa_decimate()` run in place with `ssim.c`'s kernel:

| Input | Scales | Mismatching samples |
|---|---|---|
| 320x180 to 323x181, 8-bit (odd widths and heights among them) | 2 to 10 | 0 |
| 855x481, 8-bit (last window centre at x = 855, outside the plane) | 5 | 0 |
| 400x224 and 401x225, 10-bit | 3, 6 | 0 |
| 400x224, 12-bit | 5 | 0 |
| 400x224 and 401x223, 16-bit | 7, 10 | 0 |
| 640x360, 8-bit and 16-bit | 16 | 0 |
| 1408x1408, 16-bit (the exactness bound) | 128 | 0 |

With an fp32 window sum planted in the kernel the test reports 4947 mismatching samples at 320x180, scale 3, and `test_cuda_float_ssim_parity` fails on the same case (0.99612152576446533 against 0.99612158536911011).

### fp32 convolution sums were the whole remaining difference

Before this change the two Gaussian passes accumulated in fp32, and NVCC fused each tap into an FMA. `iqa_convolve()` rounds each product to fp32, adds it to a `double` sum and rounds the sum once. On the Netflix pair at scale 1 that left 0 of 48 frames identical to the CPU, 1.788e-7 at worst. With `__dadd_rn(sum, (double)__fmul_rn(sample, weight))` in both passes, every frame is identical. The frame sum is still reduced per block and then on the host; the fp32 rounding of the mean absorbs that on every frame measured.

### Score parity against `--backend cpu`

Every case ran the twin (`feature_backends` names `float_ssim_cuda`) with no warning. 553 frames, 946 score values, all identical:

| Fixture | Request | Frames identical | Max abs diff |
|---|---|---|---|
| Netflix 576x324 8-bit | `--backend cuda --feature float_ssim` (auto 1) | 48/48 | 0 |
| Netflix 576x324 8-bit | `scale=2`, `3`, `5`, `10` | 48/48 each | 0 |
| Netflix 576x324 8-bit | `scale=3:enable_lcs:enable_db:clip_db` (score, l, c, s) | 48/48 each | 0 |
| Netflix 576x324 10 / 12 / 16-bit | auto 1, `scale=3`, `scale=7:enable_lcs` | 3/3 each | 0 |
| Checkerboard 1920x1080, 1 px and 10 px shift | auto 4 | 3/3 each | 0 |
| BBB 1920x1080 8-bit | auto 4; `scale=1` | 24/24 each | 0 |
| BBB 1920x1080 10-bit | auto 4; `enable_lcs:enable_db:clip_db` | 24/24 each | 0 |
| BBB 3840x2160 8-bit | auto 8; `enable_lcs:enable_db:clip_db` | 50/50 each | 0 |
| BBB 3840x2160 8-bit | `scale=1` | 12/12 | 0 |
| BBB 3840x2160 10-bit | auto 8; `scale=3` | 12/12 each | 0 |

`scripts/dev/speed_gpu_parity.py --backend cuda --feature float_ssim` (no tolerance) reports 48/48 and 50/50 bit-identical.

### Time per frame

Through the CLI on BBB 3840x2160 8-bit, `(t(200) - t(2)) / 198`, median of 3, each run inside the device lock, load average 16 to 19 (other jobs on the host):

| Configuration | Before (master) | After |
|---|---|---|
| `--backend cuda --feature float_ssim` | 18.85 ms (CPU fallback, default threads) | 3.01 ms (twin) |
| `--backend cpu --threads 16 --feature float_ssim` | 10.98 ms | 10.98 ms |
| `--feature float_ssim_cuda=scale=1` | 3.10 ms | 3.89 ms |
| `--backend cpu --threads 16 --feature float_ssim=scale=1` (`(t(22) - t(2)) / 20`) | 43.83 ms | 43.83 ms |
| CLI floor: `psnr_cuda` | — | 3.70 ms |

`speed_gpu_parity.py` (`(t(22) - t(2)) / 20`, median of 3) printed 0.74 ms for the CPU on 16 threads and 0.40 ms for the twin at 576x324, and 10.98 ms and 2.92 ms at 3840x2160.

GPU time per 3840x2160 frame from a CUPTI activity trace (an injection library that sums kernel durations by name), 100 frames so the device leaves its idle clock, median of 3 traces:

| Kernel | Automatic scale 8 | `scale=1` before | `scale=1` after |
|---|---|---|---|
| `calculate_ssim_decimate_8bpc` | 28.6 us | — | — |
| pass 1 (`horiz_planes` / `horiz_8bpc`) | 25.2 us | 175.1 us | 1517.8 us |
| pass 2 (`vert_combine`) | 33.8 us | 534.0 us | 2061.2 us |
| Sum | 87.6 us | 709.1 us | 3579.0 us |

At the automatic scale the SSIM stage runs on a 480x270 plane and the double sums cost microseconds. At an explicit `scale=1` they run over 8.3 million pixels: 2.9 ms more GPU time per frame, 0.8 ms more per frame through the CLI, where the frame upload dominates. A 20-frame trace showed every kernel 6 to 8 times slower because the idle device (P8, 210 MHz) had not clocked up; short traces are not usable for this.

`ptxas -v` on sm_89: 34 to 43 registers per kernel, no stack frame, no spills. The kernels take the two pictures by value and read `data[0]` and `stride[0]` only, so nvcc does not copy them to the stack.

`compute-sanitizer` memcheck and racecheck report nothing on `test_cuda_float_ssim_decimate` and `test_cuda_float_ssim_parity`.

## Alternatives explored

- Keeping the fp32 passes: inside the 5e-5 tolerance the state row asked for, but 1 to 3 fp32 ulps from the CPU on every frame for no reason a user could see. Rejected for the default path, where exactness is free.
- An exact int64 sum of the 11 products of a pass when their exponents span at most 25 binades (every partial `double` sum is then exact, so any exact method gives the CPU's value), with the fp64 loop as the fallback: fp32-rate cost in the common case. Not built: a second arithmetic path whose fallback only rare inputs reach. It is the candidate if `scale=1` at 4K needs the 2.9 ms back.
- Float-float sums as in the SYCL twin: about 2^-46 from the CPU's sum, not provably its fp32 rounding.

## Open questions

- The frame sum is a per-block tree, the CPU's is row-major. Comparing the accumulated rounding of a sequential `double` sum (about 2e-14 relative over the 262 000 terms of the largest automatic-scale plane, 2e-13 over a 4K plane at `scale=1`) with the 6e-8 spacing of fp32 means puts the chance that a frame's mean differs below 1e-6 at the automatic scale and below 1e-5 at `scale=1` on 4K; no measured frame did. A row-major device sum would remove it and was not attempted.
- The clang CUDA path (`-Denable_nvcc=false`) was not built.

## Related

- [ADR-1399](../adr/1399-cuda-float-ssim-device-decimation.md), [ADR-1370](../adr/1370-sycl-float-ssim-device-decimation.md), [ADR-1373](../adr/1373-cuda-twin-cpu-option-parity.md), [ADR-1324](../adr/1324-gpu-float-ssim-auto-scale-fallback.md), [ADR-1359](../adr/1359-cli-feature-backend-twin.md).
- Tests: `core/test/test_cuda_float_ssim_decimate.c`, `core/test/test_cuda_float_ssim_parity.c` (+ `_large`), `core/test/test_cuda_kernel_source_contract.py`, `core/test/test_gpu_float_ssim_auto_scale_contract.py`, `core/test/test_feature_backend_twin.c`, `core/tools/test/test_vmaf_feature_backend.sh`.
