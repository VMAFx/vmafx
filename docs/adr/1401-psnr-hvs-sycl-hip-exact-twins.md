<!-- markdownlint-disable MD013 MD060 -->
# ADR-1401: psnr_hvs_sycl and psnr_hvs_hip return the CPU's scores bit for bit; the fp64-free masking threshold is an integer square root

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: `gpu-parity`, `numerics`, `sycl`, `hip`, `psnr-hvs`, `testing`, `ci`, `rc3`, `fork-local`

## Context

[ADR-1397](1397-psnr-hvs-twins-cpu-float-sum.md) decided that the GPU `psnr_hvs` twins return the CPU extractor's scores bit for bit: the kernel stores the 64 values `calc_psnrhvs()` adds per block, computed in the CPU's arithmetic, and `core/src/feature/psnr_hvs_score.c` adds them into one running `float` in the CPU's order. It converted `psnr_hvs_cuda` and left `psnr_hvs_sycl` and `psnr_hvs_hip`, whose kernels were being rewritten (#1657, #1658). Both still summed each block on the device: on an Arc A380 and a gfx1036 they were 8.4e-5 dB from the CPU on the Netflix 576x324 pair and 1.1e-2 dB (8 bits) and 1.7e-2 dB (10 bits) on 3840x2160 content, against a tolerance of 3.34e-3 dB there. ADR-1397 also requires a measurement and an ADR before a twin is listed in `EXACT_TWINS`, the table that makes a parity-gate cell an equality.

The HIP kernel can take the CUDA kernel's arithmetic unchanged. The SYCL kernel cannot. It has no fp64 ([ADR-0220](0220-sycl-fp64-fallback.md): Arc A-series devices lack it, and one fp64 instruction blocks the kernel there), and on the `xe` kernel driver it must not use scratch memory (private arrays or spilled registers return wrong values, [ADR-1395](1395-sycl-kernels-no-scratch.md)). The CPU's masking threshold is `sqrt((double)s_mask * s_gvar) / 32.f` stored as `float`: the product of two `float` values taken exactly (48 bits), its square root rounded to `double`, and that rounded to `float`. A `float` product and root differ from it for a third of all operand pairs, and with the CPU-order sum in place that alone moves 3 of the 48 Netflix frames, by up to 4.4e-7 dB ([Research-1401](../research/1401-psnr-hvs-sycl-hip-exact-twins.md)).

## Decision

We will make `psnr_hvs_sycl` and `psnr_hvs_hip` store the 64 terms of every block and call the helpers of `psnr_hvs_score.h`, and list both in `EXACT_TWINS`.

**HIP.** `psnr_hvs_score.hip` takes the masking table from the CPU's `double` product (evaluated at compile time), the threshold from a `double` product and square root, and the coefficient error from the integer difference; the module is built with `-ffp-contract=off -fhip-fp32-correctly-rounded-divide-sqrt`.

**SYCL.** The masking table is the same compile-time constant, so the kernel only reads `float` data. The threshold comes from a new `sqrt_prod_rn(a, b)` in `core/src/feature/sycl/sycl_exact_fp.h`: it multiplies the two 24-bit significands as integers, takes the floor of the square root of that 48- to 50-bit product in 25 integer steps, and rounds it to the nearest `float`. This equals the CPU's value because the square root of a 48-bit integer is either a `float` or at least 2^-27 of a `float` spacing away from the midpoint of two neighbours, while rounding to `double` first moves it by at most 2^-30 of that spacing; no tie can occur. Each work-item forms its own image's threshold and the pair exchanges that one value. The terms are written straight to the device buffer, and the translation unit keeps the strict floating-point line of [ADR-1367](1367-sycl-strict-fp-every-feature-tu.md) (no contraction, correctly rounded division).

**Gate.** `EXACT_TWINS["psnr_hvs"]` is `cuda`, `sycl`, `hip`. Every `psnr_hvs` cell of the parity gate is then compared with tolerance 0 at `--precision max`. The ADR-1361 tolerance remains for a twin outside the table (the Metal twin, which is not a gate backend). The equality is between runs of one `vmaf` binary: the dB value goes through the host's `log10`, and Intel's `libimf` (an `icx` build) and glibc (a gcc build) differ by one unit in the last place on a few frames, for the CPU extractor and the twins alike.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Integer product and integer square root (this ADR) | Exact by construction, independent of the device's `sqrt` and `fma`; no fp64; no scratch memory; the argument for the double rounding fits in a comment | 25 steps of 64-bit integer arithmetic per work-item | Chosen |
| fp32-pair product and a pair square root (the sketch in ADR-1397 and Research-1397) | Stays in the arithmetic `sycl_exact_fp.h` already has (`two_prod`) | The residual of a candidate root against the midpoint of two `float` values needs more than 24 bits, so the deciding comparison has to be carried in pairs as well; correctness rests on the device's `fma` and on a case analysis per binade | More code and a longer proof for the same result |
| Device `sqrt` of the converted product, corrected by integer comparisons | Fewer integer operations than 25 steps | Needs an error bound on the device's `sqrt` and a bounded correction loop that is wrong when the bound is exceeded | A tuning candidate once correctness is in place, not the starting point |
| Take the threshold on the host | The host has fp64 | The terms depend on the threshold, so the device would need a second dispatch per frame after a round trip, or the host would compute the terms | Two round trips per frame, or no device work left |
| Keep the `float` product and root and give the cell a small tolerance | No new arithmetic | 3 of 48 frames at 576x324 and 1 of 24 at 1920x1080 and at 3840x2160 differ, by up to 4.4e-7 dB; the cell is no longer an equality, and a tolerance has to be argued per frame size again | Contradicts ADR-1397 |
| Exact threshold only on SYCL devices with fp64 | Simple on those devices | Two contracts for one twin, selected by hardware; the Arc A380 that the fork verifies on has none | One twin, one answer |

## Consequences

- **Positive**: on an Arc A380 (`xe` driver) and on a gfx1036, every frame of the Netflix 576x324 pairs (8, 10 and 12 bits, 4:2:0 and 4:2:2), the 1920x1080 checkerboard pairs and BBB at 1920x1080 and 3840x2160 (8 and 10 bits) has the same bits as `--backend cpu` on `psnr_hvs`, `psnr_hvs_y`, `psnr_hvs_cb` and `psnr_hvs_cr`; the parity gate reports 0 on all 200 frames of BBB 3840x2160. Every `psnr_hvs` cell of the gate is an equality, so a twin defect of any size fails it. The SYCL kernel reports `private_mem_size` 0 and `spill_memory_size` 0 on the A380 at SIMD16 and at forced SIMD32, and `test_sycl_kernel_scratch` keeps it off the scratch ratchet list. `sqrt_prod_rn()` matches the host's fp64 expression on 1.3 million operand pairs on the device (`test_sycl_fp_arith_contract`) and on 450 million on the host.
- **Negative**: both twins are slower, and slower than sixteen CPU threads at every size measured. Milliseconds per frame (medians over several passes of three runs each, on a host that other work shared), before and after: SYCL on the A380 0.41 and 0.61 at 576x324, 5.4 and 9.2 at 1920x1080, 22.4 and 35.9 at 3840x2160; HIP on the gfx1036 0.39 and 0.65, 3.9 and 8.7, 18.3 and 37.9; `--backend cpu --threads 16` takes 0.16, 1.7 and 6.7. The term buffer needs 256 bytes per block on the device and on the host (65 MB at 3840x2160). The scores of both twins change in their last digits, by up to 1.7e-2 dB at 3840x2160.
- **Neutral / follow-ups**: `T-SYCL-HIP-PSNR-HVS-EXACT-SUM-THROUGHPUT-2026-10-01` tracks the tuning, with the candidates of `T-CUDA-PSNR-HVS-EXACT-SUM-THROUGHPUT-2026-10-01`. Both twins still refuse 4:0:0 input, which the CPU extractor and the CUDA twin score on luma (`T-SYCL-HIP-PSNR-HVS-YUV400-REFUSED-2026-10-01`). One of 132 runs of the HIP twin on 24 frames of 3840x2160 10-bit ended in a GPU memory access fault raised by the copy engine (`T-HIP-GFX1036-SDMA-READ-FAULT-2026-10-01`); it did not recur, and its cause is not established. The Metal twin is unchanged. Guards: `test_sycl_psnr_hvs_parity`, `test_hip_psnr_hvs_parity` (bit-identity, on a device, sharing `core/test/psnr_hvs_twin_parity.h` with the CUDA test), `test_sycl_fp_arith_contract` (`sqrt_prod_rn()` on a device) and `test_psnr_hvs_twin_exact_sum_contract.py` (device-free).

## References

- Source: `req` (maintainer, popup answer of 2026-10-01, recorded in ADR-1397): "2, we tune afterwards, thats fixing now, what is the speed worth if the results are wrong".
- Implements [ADR-1397](1397-psnr-hvs-twins-cpu-float-sum.md) for the two remaining gate backends. Related: [ADR-1361](1361-psnr-hvs-area-scaled-parity-tolerance.md) (the tolerance these twins leave), [ADR-0220](0220-sycl-fp64-fallback.md) (fp64-free SYCL kernels), [ADR-1367](1367-sycl-strict-fp-every-feature-tu.md) (strict SYCL floating point), [ADR-1369](1369-sycl-shared-planes-light-twins.md) (the kernel layout), [ADR-0214](0214-gpu-parity-ci-gate.md) (cross-backend gate), [ADR-0024](0024-netflix-golden-preserved.md) (golden assertions). [ADR-1395](1395-sycl-kernels-no-scratch.md) (SYCL kernels use no scratch memory).
- [Research-1401](../research/1401-psnr-hvs-sycl-hip-exact-twins.md) — measurements, the rounding argument, ablations and cost.
- `docs/state.md`: `T-SYCL-PSNR-HVS-EXACT-SUM-2026-10-01` and `T-HIP-PSNR-HVS-EXACT-SUM-2026-10-01` (closed by this decision).
