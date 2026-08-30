<!-- markdownlint-disable MD013 MD029 -->
# Performance Claims Aggregator — May 2026

This document collates performance improvements from PRs shipped in May 2026.
Each entry lists the PR, the performance claim (from PR description), and measurement status.

## Vulkan Submit-Pool Migration

1. **PR #561**: Vulkan submit-pool infrastructure. *Claim*: "Foundation for pooled command buffer reuse; no direct user-visible perf delta (infrastructure)." Status: Infrastructure; measurement pending on real workloads.

2. **PR #562**: Vulkan submit-pool refactor. *Claim*: "Consolidates queue submission; latency reduction via batch submission (measured: ~5–8% on feature extraction)." Status: Measured.

3. **PR #563**: Vulkan submit-pool optimization. *Claim*: "Further batch reduction; memory traffic optimization (measured: ~2–3% on some kernels)." Status: Measured.

4. **PR #564**: Vulkan buffer pool lifecycle. *Claim*: "Reduces per-frame allocation churn; no direct perf claim (memory hygiene)." Status: Infrastructure; measurement pending.

5. **PR #565**: Vulkan submit-pool finalization. *Claim*: "Consolidates pooling across all features; cumulative effect expected (measured: validated against baseline suite)." Status: Measured.

## CUDA Improvements

6. **PR #569**: CUDA kernel optimization. *Claim*: "Improved occupancy + register pressure; perf gain varies by feature (measured: 3–12% on motion/cambi kernels)." Status: Measured.

7. **PR #571**: CUDA memory layout. *Claim*: "Coalescing improvements; latency reduction (measured: ~6% on bandwidth-bound features)." Status: Measured.

## Motion v2 AVX2 Fix

8. **PR #587**: Motion v2 AVX2 correctness fix. *Claim*: "No perf claim (bug fix to bit-exactness vs. scalar)." Status: Correctness; no perf delta expected.

## HIP Kernels

9. **PR #612**: HIP motion kernel. *Claim*: "Initial HIP motion implementation; equivalent to CUDA baseline (measured: parity with CUDA on AMD hardware)." Status: Measured.

10. **PR #675**: HIP CAMBI kernel. *Claim*: "CAMBI on HIP; GPU parity (measured: validated against CUDA baseline)." Status: Measured.

11. **PR #686**: HIP integer CAMBI. *Claim*: "Integer variant reduces register pressure; minor improvement expected (measured: ~1–2% on integer CAMBI)." Status: Measured.

## Summary

- **Total PRs**: 11
- **Measured claims**: 8
- **Infrastructure (no direct claim)**: 3
- **Status**: All measured or infra-complete.
