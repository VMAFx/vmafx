<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1358: The SYCL SpEED twins are device-resident, with the 25x25 linear algebra on the device and exact fp32 arithmetic

- **Status**: Accepted
- **Date**: 2026-09-29
- **Deciders**: lusoris
- **Tags**: `sycl`, `speed`, `gpu`, `performance`, `numerics`, `fork-local`

## Context

[ADR-0567](0567-speed-chroma-temporal-real-gpu.md) split every SpEED GPU twin at
the 25x25 linear algebra: the device computed covariance and tile statistics,
the host ran the eigenvalue problem, the regularity decision, the QR
factorisation and the Q^T B multiply between device passes. The SYCL twins also
kept the picture conversion, the anti-alias filter and the 16x decimation on the
host, and read the means back to the host after
`T-SYCL-SPEED-CHROMA-V-PARITY-2026-09-16`. Per frame, `speed_chroma_sycl` waited
on the queue 11 times and `speed_temporal_sycl` 9 times. At 3840x2160 the B580
twin was slower than the CPU extractor, and neither twin matched the CPU score
exactly: 1-9 of 48/50 frames were bit-identical, the worst differed by 4.2e-5.

The maintainer's requirement (RC3 performance) is that there be no GPU/CPU round
trips per frame. Moving the linear algebra to the device has to respect
[ADR-0220](0220-sycl-fp64-fallback.md) (no fp64 in any SYCL kernel) and should
not trade the parity it already had for speed.

## Decision

We run the whole per-frame SpEED chain on the device for both SYCL twins, in one
shared translation unit, `core/src/feature/sycl/speed_sycl_pipeline.cpp`:
picture conversion and temporal difference, optional prescale, the anti-alias
filter evaluated only at the decimated sample points, local mean subtraction and
the independent term, the 25 means, the covariance matrix, Householder
tridiagonalisation with the implicit-shift QR sweep, the regularity decision,
Householder QR factorisation, the Q^T B multiply with back substitution, the
variances and entropies, and the frame score. Each extractor copies its raw
planes to pinned staging in `submit()`, enqueues one upload and the chain
without waiting, and reads one `FrameResult` in `collect()`. The chain is
recorded once per channel binding as a SYCL command graph (one for chroma, two
for the alternating temporal slots) and replayed as a single submission; a
device without graph support gets the same kernels enqueued directly.

Every routine mirrors its CPU reference operation for operation in fp32:

- The speed TUs are compiled with `-ffp-contract=off` after `-fp-model=precise`
  (`sycl_speed_strict_fp_args` in `core/src/meson.build`); precise alone still
  contracts FMAs in kernel lambdas.
- Division and square root go through `div_rn()` / `sqrt_rn()`: the hardware
  approximation, one refinement, and an exact-residual choice between the two
  adjacent floats, falling back to `sycl::ext::intel::math::fdiv_rn` /
  `fsqrt_rn` when the residual signs do not prove the bracket.
- `log2f` is evaluated in fp32 pairs and rounded once.
- The reference's fp64 expressions have exact fp32 forms: the `EIGENVALUE_EPS`
  comparisons use `1e-6 == 0x1.0c6f7ap-20f + 0x1.6bdb1ap-49f`, the covariance
  sum is carried in exact fp32 pairs, the prescale coordinate is an exact fp32
  pair.
- Host constants (filter taps, `log2f(2 pi e)`, the entropy floor) are computed
  once at init by the same C code the CPU extractor uses
  (`speed_internal_entropy_constant`, `speed_internal_base_entropy`,
  `vif_tools.c`).

The CUDA and HIP twins keep the ADR-0567 split until the same algorithm is
ported there; each is an RC3 row in `docs/state.md`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the host linear algebra, batch the four channels into one round trip per frame | Small change; linear algebra stays bit-identical to `speed_internal.c` | Still a per-frame round trip, which is the requirement; the filter and decimation stay on the host, which is most of the 4K cost | Does not meet the requirement |
| Device linear algebra with the SYCL defaults (`/`, `sqrt`, `sycl::log2`, `-fp-model=precise`) | Simplest device port | Contracted FMAs and non-correctly-rounded division and square root move eigenvalues and can flip the regular/singular decision; parity no better than before | Trades exactness for speed |
| Device linear algebra with `-foffload-fp32-prec-div/-sqrt` | Correct rounding from the compiler | The flags act only on the final image link; applying them there changes every SYCL extractor's numerics | Out of scope for the speed twins; the math-extension fallback plus residual rounding is exact per TU |
| Device linear algebra with `fdiv_rn` / `fsqrt_rn` everywhere | Exact, no custom code | About 15x a hardware division; the eigenvalue kernel went from 0.38 ms to 5.7 ms per frame | Too slow as the only path; kept as the fallback |
| **Device-resident chain, exact fp32 arithmetic as above (chosen)** | No per-frame host compute or mid-frame wait; bit-identical to the CPU extractor on every tested frame | More device code than the split (one shared TU of about 2 000 lines); the eigenvalue sweep is latency-bound on one work item | Chosen |

## Consequences

- **Positive**: `speed_chroma_sycl` and `speed_temporal_sycl` match
  `--backend cpu` bit for bit on the Netflix 576x324 pair and 50 frames of BBB
  4K on the Arc B580 and the UHD 770. On the B580, ms/frame before -> after:
  `speed_chroma` 23.31 -> 7.51 at 4K and 3.60 -> 0.89 at 576x324;
  `speed_temporal` 60.37 -> 7.58 at 4K and 2.19 -> 0.83 at 576x324 (CPU with 16
  threads: 7.24, 0.16, 37.27, 0.85; full table in
  [`docs/metrics/speed_qa.md`](../metrics/speed_qa.md)). One implementation
  serves both extractors (HISS-19).
- **Negative**: the tridiagonal QR sweep runs on one work item in the
  reference's operation order, so at 576x324 the twins stay slower than the CPU
  extractor (about 3.5 ms per frame on the UHD 770). Prescale with `lanczos4` evaluates its kernel weights with fp32
  `sinpi` where the CPU uses fp64 `sin`, so that option is within the ADR-0214
  tolerance but not bit-identical. AdaptiveCpp builds have no math-extension
  fallback and degrade to the ADR-0214 tolerance.
- **Neutral / follow-ups**: port the algorithm to the CUDA and HIP twins
  (`T-CUDA-SPEED-HOST-RESIDUAL-2026-09-29`, `T-HIP-SPEED-HOST-RESIDUAL-2026-09-29`);
  the finding that `-fp-model=precise` does not stop device FMA contraction and
  that device division is not correctly rounded applies to every SYCL kernel
  (`T-SYCL-FP-MODEL-PRECISE-CONTRACTS-2026-09-29`).
  `core/test/test_sycl_kernel_source_contract.py` now guards the fp64-free
  pipeline, the absence of the host residual and of mid-frame waits.

## References

- `req` (maintainer, 2026-09-29): "there shouldnt be any gpu cpu rountrips" /
  "remove the host roundtrips and then test again".
- [ADR-0567](0567-speed-chroma-temporal-real-gpu.md) (the split this replaces
  for SYCL), [ADR-0964](0964-implement-speed-internal-and-wire-gpu-speed-extractors.md),
  [ADR-1218](1218-gpu-speed-singular-device-solution.md),
  [ADR-0220](0220-sycl-fp64-fallback.md), [ADR-0214](0214-gpu-parity-ci-gate.md),
  [ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md),
  [ADR-1352](1352-rc-phase-shift-plus-one.md).
- [Research-1358](../research/1358-sycl-speed-device-resident.md).
