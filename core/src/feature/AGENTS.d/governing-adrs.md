---
paths:
  - core/src/feature/feature_extractor.cpp
  - core/src/feature/feature_collector.cpp
invariant: Governing ADR index and compliance contracts for feature extractors.
area: framework
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Governing ADR Index for Feature Subsystem

## Governing ADRs

- [ADR-0125](../../../../docs/adr/0125-ms-ssim-decimate-simd.md) —
  MS-SSIM decimate separable SIMD + bit-exactness contract.
- [ADR-0126](../../../../docs/adr/0126-ssimulacra2-feature-extractor.md) +
  [ADR-0130](../../../../docs/adr/0130-ssimulacra2-scalar-implementation.md)
  — SSIMULACRA 2 extractor scope + scalar implementation.
- [ADR-0138](../../../../docs/adr/0138-iqa-convolve-avx2-bitexact-double.md) —
  `iqa_convolve` widen-then-add bit-exactness pattern.
- [ADR-0139](../../../../docs/adr/0139-ssim-simd-bitexact-double.md) —
  SSIM accumulate per-lane scalar-double reduction pattern.
- [ADR-0140](../../../../docs/adr/0140-simd-dx-framework.md) — SIMD DX
  framework (`simd_dx.h` + `/add-simd-path` skill upgrade).
- [ADR-0182](../../../../docs/adr/0182-gpu-long-tail-batch-1.md) +
  [ADR-0188](../../../../docs/adr/0188-gpu-long-tail-batch-2.md) +
  [ADR-0192](../../../../docs/adr/0192-gpu-long-tail-batch-3.md) —
  GPU long-tail batches 1–3. Every registered feature extractor
  now has at least one GPU twin (lpips remains ORT-delegated).
- [ADR-0193](../../../../docs/adr/0193-motion-v2-vulkan.md) —
  `motion_v2` Vulkan kernel. ADR-0662 corrects its mirror contract:
  `integer_motion_v2.c::mirror` uses reflect-101 (`2 * size - idx - 2`)
  and CUDA / SYCL twins must keep that literal aligned
  with CPU reference.
- [ADR-0205](../../../../docs/adr/0205-cambi-gpu-feasibility.md) +
  [ADR-0210](../../../../docs/adr/0210-cambi-vulkan-integration.md) —
  cambi Vulkan integration (Strategy II, hybrid host/GPU).
  Precision-sensitive `calculate_c_values` + top-K stay on host;
  GPU phases are integer + bit-exact.
- [ADR-0214](../../../../docs/adr/0214-gpu-parity-ci-gate.md) —
  GPU-parity CI gate: per-feature `FEATURE_TOLERANCE` map in
  `scripts/ci/cross_backend_parity_gate.py` is single source of
  truth. Every new GPU twin needs entry.

## Newly-arrived shipped surfaces (rebase awareness)
