<!-- markdownlint-disable MD013 MD060 -->
# ADR-1399: float_ssim_cuda decimates and convolves on the device with the CPU's arithmetic

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: cuda, gpu, ssim, performance, numerics, gpu-parity, rc3, fork-local

## Context

CPU `float_ssim` picks a decimation factor from the short side, `max(1, round(min(w, h) / 256))`: 1 below 384 px, 4 at 1920x1080, 8 at 3840x2160. `ssim.c` low-passes both planes with a `scale x scale` box of weight `1.0f / (scale * scale)` and decimates them with `iqa_decimate()` before `iqa_ssim()`. `float_ssim_cuda` implemented scale 1 only, so the [ADR-1324](1324-gpu-float-ssim-auto-scale-fallback.md) context check refused every picture with a short side of 384 px or more, and model dispatch and `--backend cuda --feature float_ssim` ([ADR-1359](1359-cli-feature-backend-twin.md)) computed the feature on the CPU (`T-CUDA-FLOAT-SSIM-SCALE-GT1-2026-09-29`). [ADR-1370](1370-sycl-float-ssim-device-decimation.md) closed the same gap for SYCL.

[ADR-1373](1373-cuda-twin-cpu-option-parity.md) had made the twin's per-pixel `l * c * s` and its frame mean the CPU's, but left the two Gaussian passes accumulating in fp32 (with NVCC's default FMA contraction), where `iqa_convolve()` adds each fp32 product to a `double` sum and rounds once. On the Netflix 576x324 pair that left every frame 1 to 3 fp32 ulps from the CPU (0 of 48 frames identical, 1.8e-7 at worst), and ADR-1373 listed the double convolution as a measured follow-up.

Constraints: the decimated planes must be the CPU's byte for byte; no host pass and one result read-back per frame; every CPU option keeps working at every scale; one behaviour, one implementation (HISS-19).

## Decision

We will run the CPU pipeline on the device, stage for stage and rounding for rounding:

1. **Decimation kernel.** Above scale 1, `calculate_ssim_decimate_{8,16}bpc` reads the device picture's luma and writes two fp32 planes. Each output is `iqa_filter_pixel()` at `(x * scale, y * scale)`: offsets `-scale/2 .. scale-1-scale/2`, `KBND_SYMMETRIC` edges, `picture_copy()`'s divisor as an exact reciprocal, and the CPU's fp32 product `sample * (1.0f / (scale * scale))`. The window is summed in int64 units of 2^-52 and converted to fp32 once with `__ll2float_rn()`; by [Research-2130](../research/2130-sycl-float-ssim-device-decimation.md) that is the CPU's `(float)(double sum)` bit for bit up to scale 128. The plane size comes from the shared `iqa/decimate_dim.h`.
2. **The gate refuses only what the device cannot compute exactly.** `check_context_cuda()` returns `-ENOTSUP` when the decimated plane is smaller than the 11x11 Gaussian or the scale exceeds 128, and 0 otherwise; `init` refuses the same geometry through the same predicate with `-EINVAL`.
3. **Both Gaussian passes add in `double`.** Every tap is `__dadd_rn(sum, (double)__fmul_rn(sample, weight))` and each pass result is rounded to fp32 once, as `iqa_convolve()` does. Scale 1 keeps reading the device picture directly (no extra plane); above scale 1 a third pass-1 kernel reads the decimated planes. All three share one templated body.

With ADR-1373's combine, every per-pixel value is then the CPU's. The frame sum stays a per-block reduction.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| int64 decimation + double convolution sums (chosen) | Decimated planes byte-identical; every frame of every tested fixture equals the CPU; one arithmetic for all depths and scales | The two passes cost 110 fp64 adds per SSIM-plane pixel: 2.9 ms of GPU time per 3840x2160 frame at an explicit `scale=1` | — |
| Keep the fp32 passes (the row's 5e-5 contract) | Fastest at `scale=1`; smallest change | 1 to 3 fp32 ulps from the CPU on every frame; `enable_db` magnifies it near 1; the twin and the CPU keep differing for no reason a user can see | The cost falls on a non-default option; at the automatic scale the SSIM plane is at most 383 px on its short side and the fp64 adds cost microseconds |
| Exact int64 sum of the 11 products when their exponents span at most 25 binades, fp64 otherwise | The CPU's value at fp32 cost in the common case | A second arithmetic path whose fallback only rare inputs reach, so it is hard to keep tested | Complexity for a non-default path; a candidate if `scale=1` throughput at 4K matters |
| Float-float (two-sum) passes, as the SYCL twin | No fp64 | About 2^-46 from the CPU's sum, not its rounding; not provably the CPU's fp32 value | CUDA devices have fp64; exact is available |
| Always convert to fp32 planes, also at scale 1 (as SYCL does) | One pass-1 kernel | Two full-resolution fp32 planes written and read back per frame at scale 1 | The picture-reading kernels already exist; a template keeps one body |
| Decimate on the host, upload the small planes | No new kernel | A full-frame host pass per plane per frame, and a download of the device picture first | Violates device residency |
| Row-major sequential frame sum | The last source of difference removed | A serial pass over every pixel, or reading every per-pixel term back | The fp32 rounding of the mean absorbs the reduction order on every tested frame |

## Consequences

- **Positive**: `--backend cuda --feature float_ssim` and models run `float_ssim_cuda` at every size with no fallback warning. On an RTX 4090 every frame equals `--backend cpu` at `--precision max`: the Netflix 576x324 pair at 8, 10, 12 and 16 bits and scales 1 to 10, the 1920x1080 checkerboard pairs, a 1920x1080 pair at 8 and 10 bits and BBB 3840x2160 at 8 and 10 bits, `enable_lcs` / `enable_db` / `clip_db` included ([Research-1399](../research/1399-cuda-float-ssim-device-decimation.md)). The decimated planes equal `iqa_decimate()` byte for byte (`test_cuda_float_ssim_decimate`). At 3840x2160 the CLI takes 3.0 ms per frame instead of 18.9 with the CPU fallback (11.0 for the CPU extractor on 16 threads); the three kernels take 88 us of GPU time per frame (CUPTI).
- **Negative**: at an explicit `scale=1` the two passes run in fp64 over the full plane: 3.58 ms of GPU time per 3840x2160 frame instead of 0.71, 3.9 ms per frame through the CLI instead of 3.1 (the CPU extractor at `scale=1`: 43.8 ms on 16 threads). Default output of `float_ssim_cuda` moves by up to 3 fp32 ulps of the mean, onto the CPU's value. The frame sum is still reduced per block, so a frame whose mean lies within about 1e-13 (relative) of an fp32 rounding boundary could differ from the CPU by one ulp; none of the 553 frames measured does.
- **Neutral / follow-ups**: the HIP and Metal twins stay scale 1 only (`T-HIP-FLOAT-SSIM-SCALE-GT1-2026-09-29`, `T-METAL-FLOAT-SSIM-SCALE-GT1-2026-09-29`). Guarded by `test_cuda_float_ssim_parity` (and `_large`), `test_cuda_float_ssim_decimate`, `test_gpu_float_ssim_auto_scale_contract`, `test_cuda_kernel_source_contract`, `test_feature_backend_twin` and `test_vmaf_feature_backend_cuda`. A change to `ssim.c`'s low-pass, `iqa/decimate.c` or `iqa/convolve.c` changes `core/src/feature/cuda/integer_ssim/ssim_score.cu` in the same PR.

## References

- req: RC3 CUDA lane brief (2026-10-01): "Implement the decimated scales on the device, mirroring the CPU pipeline bit for bit the way the SYCL twin does (ADR-1370: ssim.c box low-pass + iqa/decimate.c::iqa_decimate(), shared iqa/decimate_dim.h). #1637 (merged) already made the scale-1 arithmetic match the CPU (per-pixel l*c*s in the CPU's types, reduced in double, frame mean rounded to fp32): keep that contract at every scale. Verify on the 4090 at 576x324, 1080p and 4K, 8 and 10 bit."
- req: RC3 worker rules (2026-10-01): "Correctness before speed: a GPU twin matches the CPU extractor bit for bit where the row or its ADR says so, otherwise within the stated tolerance".
- [Research-1399](../research/1399-cuda-float-ssim-device-decimation.md) — parity and timing tables; [Research-2130](../research/2130-sycl-float-ssim-device-decimation.md) — the exactness proof of the int64 window sum.
- [ADR-1370](1370-sycl-float-ssim-device-decimation.md) — the SYCL design this ports; [ADR-1373](1373-cuda-twin-cpu-option-parity.md) — the per-pixel combine and the options; [ADR-1324](1324-gpu-float-ssim-auto-scale-fallback.md) — the context check; [ADR-1359](1359-cli-feature-backend-twin.md) — `--feature` twin routing; [ADR-1215](1215-cuda-psnr-16bpc-plane-argument.md) — kernel argument lists; [ADR-1206](1206-gpu-parity-large-fixture-variants.md) — large-fixture variants.
