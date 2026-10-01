<!-- markdownlint-disable MD013 MD060 -->
# ADR-1397: GPU psnr_hvs twins reproduce the CPU's running float sum bit for bit

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: `gpu-parity`, `numerics`, `cuda`, `psnr-hvs`, `testing`, `ci`, `rc3`, `fork-local`

## Context

`calc_psnrhvs()` (`core/src/feature/third_party/xiph/psnr_hvs.c`, and its AVX2 and NEON copies) adds every masked coefficient error of a plane into one running `float`: 64 terms per 8x8 block, 10 802 176 terms for a 3840x2160 luma plane. The rounding of each addition depends on the sum so far, so the result is a property of the order and the type of the accumulation, not only of the terms.

The GPU twins sum the 64 terms of a block on the device and the blocks on the host. That is a different order, and it is closer to the exact sum: at 3840x2160 the CPU's value is up to 1.10e-2 dB from the same sum taken in `double`, the CUDA twin within 5.1e-5 dB. [ADR-1361](1361-psnr-hvs-area-scaled-parity-tolerance.md) scaled the parity tolerance with the term count under a random-rounding model (3.34e-3 dB at 3840x2160). The CPU's error is a bias, not a random walk: on 24 frames of the BBB 3840x2160 fixture the CUDA twin is 1.10e-2 dB from the CPU (`psnr_hvs_y`), three times the tolerance, and 1.66e-2 dB at 10 bits ([Research-1397](../research/1397-psnr-hvs-twins-cpu-float-sum.md)). The gate fails the twin that is nearer the mathematical value.

Making the CPU sum in `double` removes the difference at its source, and was tried: it moves `psnr_hvs` of the Netflix 576x324 pair from 31.33044560416667 to 31.330389687500002, past the `places=4` assertion in `python/test/third_party/xiph/vmafexec_feature_extractor_test.py`. Golden assertions are not modified ([ADR-0024](0024-netflix-golden-preserved.md)), so the CPU extractor keeps its `float` sum and remains the reference.

## Decision

We will make the GPU `psnr_hvs` twins return the CPU extractor's scores bit for bit, by reproducing its accumulation instead of widening the tolerance or changing the CPU.

**Terms on the device.** A twin's kernel computes, for every block, the 64 values `calc_psnrhvs()` adds to its sum, in the CPU's arithmetic, and stores all of them (row-major, blocks in raster order). Three things in the block arithmetic differ from a plain fp32 port and are part of the contract: the masking table is `(csf * 0.3885746225901003)^2` taken in `double` and stored as `float`; the masking threshold is `sqrt((double)mask_energy * variance_ratio) / 32` rounded to `float` once; and no multiply is fused with an add, because the CPU reference is built with contraction off. The coefficient error is the integer `abs(ref - dist)` converted to `float`.

**Sum on the host.** `vmaf_psnr_hvs_plane_score()` (`core/src/feature/psnr_hvs_score.c`) adds a plane's terms one by one into a single `float`, then divides by the term count and the squared sample maximum in `float`, as `calc_psnrhvs()` does. `vmaf_psnr_hvs_combined_score()` and `vmaf_psnr_hvs_score_db()` are the CPU `extract()` expressions. The file is built in the strict floating-point library of the scalar reference. Every twin calls these helpers; no twin keeps a sum of its own.

**Scope of this change.** `psnr_hvs_cuda` is converted here. `psnr_hvs_hip` and `psnr_hvs_sycl` are being rewritten in open pull requests (#1658, #1657) and follow once those land; until then they keep their per-block sums.

**Gate.** `EXACT_TWINS` in `scripts/ci/cross_backend_calibration.py` lists the twins that carry this contract (`psnr_hvs`: `cuda`). A cell whose two sides are the CPU extractor or a listed twin is compared with tolerance 0 at `--precision max`, at every frame size, ahead of any calibration row. The ADR-1361 area-scaled tolerance stays the contract of a twin that is not listed. This amends ADR-1361: its rejected alternative "emulate the CPU's term order on the host" is the design of a listed twin, and its tolerance no longer applies to one.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Twins reproduce the CPU's accumulation (this ADR) | One answer per input on every backend; the gate becomes an equality; no tolerance to calibrate per size | 256 bytes of readback per block (65 MB for a 3840x2160 4:2:0 frame) and a sequential host sum: 12.2 ms instead of 2.4 ms per 3840x2160 frame on an RTX 4090 | Chosen: maintainer decision, 2026-10-01 |
| Widen the ADR-1361 tolerance to cover the CPU's bias (about 2e-2 dB at 3840x2160) | No code change | The gate would accept any twin error below the CPU's own rounding error, which grows with the frame; two backends keep reporting different scores for one input | Hides the difference instead of removing it |
| Sum in `double` on the CPU | Removes the bias at its source; the per-block twins agree with that value to 5.1e-5 dB | Moves the Netflix golden `psnr_hvs` mean past `places=4` (31.33044560416667 to 31.330389687500002) | Golden assertions are not modified |
| Keep the per-block device sum and compensate on the host | Small readback | The CPU's rounding at each of its 10.8 million additions depends on the running value; block subtotals do not determine it | Not exact |
| Order-preserving reduction on the device | No large readback, no host sum | The reduction has to reproduce each rounding of a sequential `float` sum; a design exists on paper only (Research-1397 §6) | Deferred to the tuning step, after correctness |

## Consequences

- **Positive**: `psnr_hvs_cuda` equals `--backend cpu` on every frame measured, at `--precision max`: the Netflix 576x324 pairs at 8, 10 and 12 bits and in 4:2:2, the 1920x1080 checkerboard pairs, BBB at 1920x1080 and 3840x2160 at 8 and 10 bits (previously up to 1.66e-2 dB apart). The CUDA cell of the parity gate is an equality, so a twin defect of any size fails it.
- **Negative**: the twin is slower. Median of three runs on an RTX 4090, ms per frame, before and after: 0.09 and 0.29 at 576x324, 0.60 and 3.06 at 1920x1080, 2.36 and 12.16 at 3840x2160; sixteen CPU threads take 0.14, 1.7 and 6.5. The host sum is 6.4 ms of the 3840x2160 figure. The term buffer needs 256 bytes per block on the device and in pinned host memory (65 MB at 3840x2160, 259 MB at 7680x4320). The twin now reproduces a value that is further from the exact sum than the one it returned before.
- **Neutral / follow-ups**: `T-CUDA-PSNR-HVS-EXACT-SUM-THROUGHPUT-2026-10-01` tracks the tuning (15 to 32 % of a plane's terms are nonzero, and adding a zero changes nothing, so compacting them shortens both the readback and the sum). `T-HIP-PSNR-HVS-EXACT-SUM-2026-10-01` and `T-SYCL-PSNR-HVS-EXACT-SUM-2026-10-01` track the other two twins; the SYCL kernel has no fp64 on Arc A-series ([ADR-0220](0220-sycl-fp64-fallback.md)), so it needs the threshold's double product and square root in fp32 pairs. The Metal twin is outside the gate's backend list and unchanged. Guards: `test_cuda_psnr_hvs_parity` (bit-identity, on a device), `test_psnr_hvs_score` and `test_psnr_hvs_twin_exact_sum_contract.py` (device-free).

## References

- Source: `req` (maintainer, popup answer, 2026-10-01): "2, we tune afterwards, thats fixing now, what is the speed worth if the results are wrong".
- Amends [ADR-1361](1361-psnr-hvs-area-scaled-parity-tolerance.md) (area-scaled tolerance; remains for twins outside `EXACT_TWINS`). Related: [ADR-0191](0191-psnr-hvs-vulkan.md) (the 5e-4 contract), [ADR-0214](0214-gpu-parity-ci-gate.md) (cross-backend gate), [ADR-0024](0024-netflix-golden-preserved.md) (golden assertions), [ADR-1369](1369-sycl-shared-planes-light-twins.md) (the kernel layout this builds on), [ADR-0220](0220-sycl-fp64-fallback.md).
- [Research-1397](../research/1397-psnr-hvs-twins-cpu-float-sum.md) — measurements, ablations, cost breakdown and the tuning options.
- `docs/state.md`: `T-PSNR-HVS-CPU-FLOAT-SUM-4K-2026-09-30` (closed by this decision).
