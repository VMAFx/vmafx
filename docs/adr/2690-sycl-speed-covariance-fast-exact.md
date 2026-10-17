<!-- markdownlint-disable MD013 MD060 -->
# ADR-2690: The SYCL SpEED covariance replays the CPU's sequential fp64 sum, with its terms formed in parallel

- **Status**: Accepted
- **Date**: 2026-10-08 (decided and measured 2026-10-06 to 2026-10-07 on the Tualua/vmafx fork)
- **Deciders**: Lusoris Dev (direction chosen by the user, 2026-10-06)
- **Tags**: sycl, speed, bit-exactness, performance, arc-a380, fork-local

## Context

`speed.c::compute_cov_kernel_scalar()` computes each entry of SpEED's 25x25 covariance matrix as one fp64 running sum. It adds `(x - mean_x) * (y - mean_y)` in raster order and rounds every add. `compute_covariance_row()` then stores `(float)(sum / (w * h))`; the contract is in `core/src/feature/speed_cov.h` ([ADR-1459](1459-speed-cov-kernel-exact.md)).

The SYCL twin added these terms near-exactly in parallel, as fp32 pairs, and rounded once. On a real 3840x1600 10-bit frame this stored the neighbouring fp32 value for entry (14, 11) of the U-reference covariance (`0x3ae66460` against the CPU's `0x3ae6645f`), which moved `speed_chroma_u_mxv_45_nnf_0.1_snn_0.19_wvm_5` by one fp32 step, 2.384e-07 (`T-SPEED-CHROMA-SYCL-COV-1ULP-2026-10-06`). The existing parity fixtures never produce a cancelling entry, so the declared-exact gate cells did not see it.

The twin is declared exact (`scripts/ci/exact_twins.d/speed_chroma.sycl` and `speed_temporal.sycl`, [ADR-1477](1477-speed-upstream-double-math.md)). Per AGENTS.md section 11, exactness comes first in RC3 and the speed it costs is recovered in RC8, never traded for a tolerance. The kernels also have to stay:

- free of fp64 ([ADR-0220](0220-sycl-fp64-fallback.md));
- free of scratch memory ([ADR-1395](1395-sycl-kernels-no-scratch.md));
- on sub-group size 16 or 32 ([ADR-1468](1468-sycl-sub-group-sizes-every-aot-target.md));
- device-resident, with no host wait mid-frame and buffers allocated at init ([ADR-1358](1358-sycl-speed-device-resident-linalg.md)).

## Decision

**Fix (A).** `covariance_entry()` in `core/src/feature/sycl/sycl_speed_cov_math.h` performs the reference's operations in its order in soft fp64 (`sycl_soft_signed.h`): the two differences, the product, the add, then the quotient by the exact `uint64_t` element count and the conversion to fp32. The exact count keeps what `T-GPU-SPEED-COV-COUNT-FP32-2026-10-05` fixed for the pair form. A runs one work-item per (channel, entry), 1300 work-items at 3840x1600 chroma, each walking 5336 terms. It is exact, and it costs about +9.7 ms per frame on an Arc A380.

**Split chain (B).** We then split `covariance_entry()`'s operations across three launches and keep every operation and its order:

1. **Differences**, parallel over (channel, element, term). `fl64(x - mean)` is formed once for each of the 25 block elements and each pixel of the submatrix. The reference forms the same difference for every entry that uses the element, so 325 entries share 25 x n differences.
2. **Products**, parallel over (channel, term, entry). `fl64(dx * dy)` is formed from the two stored differences and stored as its fp64 bit pattern, term-major.
3. **Add chain**, one work-item per (channel, entry). It runs `signed_add()` over the stored terms in raster order from +0, then does A's quotient and fp32 conversion.

Large geometries run steps 2 and 3 in slices of whole submatrix rows, and the chain's running sum is kept as fp64 bits between slices. The per-term work leaves the 1300 sequential chains, and a chain step drops from 791 to 208 SIMD16 instructions (estimate below).

The exactness argument:

- The operations are `covariance_entry()`'s, called through the same helpers in the same order. Only where each one runs and where its result is stored change.
- A stored value is a normal fp64 value or zero in A's range, so `signed_bits()` and `signed_from_bits()` round-trip it exactly.
- A zero product is +0 in `signed_mul()` and stays +0.

`covariance_entry()` stays in the header. It is the single-function form the device probe test checks, and it calls the same shared helpers.

ISA estimate, AOT for `acm-g11` with the SYCL feature FP line, SIMD16 instructions in the loop body:

| Form | Instructions per dependent step | Chain estimate per 3840x1600 frame |
|---|---|---|
| A: `covariance_entry()`, the whole term per step | 791 | measured +9.7 ms |
| Split chain (B, this decision): `signed_add()` on a stored term | 208 (832 per 4 unrolled steps) | about 2.6 ms, plus about 1-1.5 ms of parallel term work and term traffic |
| B1: 128-bit fixed point with RNE to 53 bits | 474 | about 5.9 ms, plus the same term work |

The estimate scales the measured A cost (about 4.7 cycles per instruction: 82 SIMD16 threads cannot hide latency on 128 EUs).

## Measured outcome

Measured on an Arc A380 (i915 driver, compute-runtime 26.35.39758.10, kernel 6.19.14) with QSV zero-copy on a 3840x1600 10-bit segment and `vmaf_v1.0.16_3d0h`, on the fork branch that carried A and B (`perf/sycl-zerocopy-throughput`, Tualua/vmafx). Three interleaved, rotated rounds, video only, uninstrumented, with one unrecorded warm-up per round. The decision metric is the filter's GPU time per frame.

| Build | GPU ms per frame, median of 3 | Over the pre-fix kernel |
|---|---|---|
| Before the fix (pair sum, not exact) | 18.83 | - |
| A | 28.53 | +9.70 ms (+51.5 %) |
| Split chain (B) | 23.06 | +4.23 ms (+22.5 %) |

- **Recovery:** B recovers 5.47 of A's 9.70 ms, 56.4 % (58.9 % with audio and subtitle streams left in the harness command; an earlier three-round series gave 58.7 %).
- **Remaining cost:** about +4.2 ms per frame. VTune puts the sequential chain at 4.24 ms of the 5.17 ms the three covariance kernels take (differences 0.10, products 0.83).
- **Identity:**
  - 200 frames of the segment, QSV zero-copy against FFmpeg `libvmaf` on the CPU: identical, for A and for B.
  - `vmaf --backend cpu` against `--backend sycl` on the dumped 142-frame clip: 0 differences in any feature on any frame. On the frame that had split, `speed_chroma_u` is 3.644730567932129 on both.
- **Device tests:** `test_sycl_speed_cov_math` (host, device, and the split form on the device), the three SpEED parity tests, `test_sycl_exact_twins` and `test_sycl_kernel_scratch` (no kernel with scratch) pass. So does `scripts/dev/speed_gpu_parity.py` on 576x324.

**Decision:** B is kept. The remaining ~4.2 ms is the RC8 row `T-SYCL-SPEED-COV-EXACT-SEQUENTIAL-COST-2026-10-06`; option (c) below is its next step.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| (a) Keep A only | Exact, one kernel | +9.7 ms per frame | Section 11 asks for the recovery |
| (b) B1: terms in parallel, sequential 128-bit fixed-point chain with RNE to 53 significant bits after every add | Exact by a written argument: every running sum is a multiple of the smallest term ulp, and a width bound decides the fast path; A as fallback outside the bound | 474 instructions per step on `acm-g11`, 2.3x the split chain's: the variable 128-bit shifts and the RNE are emulated in 32-bit lanes. Needs a range check, per-entry exponent bounds and a fallback | Strictly more expensive than the chosen design, and its exactness argument has more parts |
| (c) B2: terms in parallel, chunked binade-segmented composition, extending `ordered_sum.h` ([ADR-1433](1433-cuda-ssimulacra2-cpu-sum-order.md)) to signed terms | Most parallel; could approach +1 ms | `ordered_sum.h` requires non-negative terms. Signed terms let the sum leave and re-enter a binade inside a chunk, so every chunk needs prefix minimum and maximum checks for both parities and a sequential fallback. A cancelling entry (the case found on the real frame) crosses binades many times | Follow-up (RC8) for the remaining cost |
| (d) B3: fused, the chain work-item forms each term itself | No term buffer | The product and differences stay inside the 1300 sequential chains: 791 instructions per step, which is A | No gain |
| (e) Covariance on the host | Native fp64 | Breaks the device-resident rule (a mid-frame readback and wait, ADR-1358) | Rejected |
| (f) A tolerance on the entry or the score | None | Moves score bits; forbidden by AGENTS.md section 11 and the exact-twin contract (ADR-1477) | Rejected |
| (g) The chosen split chain | Exact by construction, with no range bound and no fallback; 208 instructions per step | A 55 MB term buffer at 3840x1600 chroma (four channels), written and read once per frame; three launches instead of one | Chosen |

## Consequences

- **Positive**:
  - `speed_chroma_sycl` and `speed_temporal_sycl` return the CPU's covariance bits on cancelling entries, which the exact-twin declaration already claimed.
  - Measured recovery is 56-59 % of A's cost (see Measured outcome). No new arithmetic: the chain is A's.
- **Negative**:
  - About +4.2 ms per 3840x1600 frame on an Arc A380 against the inexact kernel (RC8 row `T-SYCL-SPEED-COV-EXACT-SEQUENTIAL-COST-2026-10-06`).
  - Two more device buffers: the differences (25 x n x 8 bytes per channel) and the term slice (325 x slice x 8 bytes per channel).
  - The chain is still sequential: about 5336 dependent adds per entry.
- **Neutral / follow-ups**:
  - The CUDA (`cuda/speed/speed_score.cu`, `covariance_partial`) and HIP (`hip/speed/speed_hip_device.h`, `speed_hd_covariance_partial`) twins keep the pair-sum design, a suspected defect not verified on a device (`T-CUDA-SPEED-COV-PAIR-SUM-SUSPECTED-2026-10-06`, `T-HIP-SPEED-COV-PAIR-SUM-SUSPECTED-2026-10-06`). They are not changed here.
  - `test_speed_cov_count_contract.py` holds the SYCL twin to the soft-fp64 quotient instead of the pair divisor.

## References

- Source: `req`, the user's decision on the debug checkpoint, 2026-10-06: «A сейчас, потом B (Recommended)».
- `docs/state.md` rows `T-SPEED-CHROMA-SYCL-COV-1ULP-2026-10-06`, `T-SYCL-SPEED-COV-EXACT-SEQUENTIAL-COST-2026-10-06`, `T-CUDA-SPEED-COV-PAIR-SUM-SUSPECTED-2026-10-06`, `T-HIP-SPEED-COV-PAIR-SUM-SUSPECTED-2026-10-06`; the count fix `T-GPU-SPEED-COV-COUNT-FP32-2026-10-05` (#2188).
- ISA estimate: a throwaway translation unit holding the three loops, AOT-compiled for `acm-g11` with IGC shader dumps; the instruction counts are in the table above.
- [ADR-0220](0220-sycl-fp64-fallback.md), [ADR-1395](1395-sycl-kernels-no-scratch.md), [ADR-1468](1468-sycl-sub-group-sizes-every-aot-target.md), [ADR-1358](1358-sycl-speed-device-resident-linalg.md), [ADR-1477](1477-speed-upstream-double-math.md), [ADR-1459](1459-speed-cov-kernel-exact.md), [ADR-1433](1433-cuda-ssimulacra2-cpu-sum-order.md).
