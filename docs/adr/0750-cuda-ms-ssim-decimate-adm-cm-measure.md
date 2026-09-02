<!-- markdownlint-disable MD013 MD060 -->
# ADR-0750: Hardware Measurement Verdict for PR perf/cuda-ms-ssim-decimate-adm-cm-ncu-driven

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: cuda, performance, ms_ssim, adm_cm, measurement

## Context

PR `perf/cuda-ms-ssim-decimate-adm-cm-ncu-driven-20260528` introduced two CUDA
optimizations authored with ncu *estimates only* (no hardware measurement due to GPU
contention at authoring time):

1. `ms_ssim_decimate` smem tiling — estimated +68–93% kernel speedup
2. `adm_cm_line_kernel_8` `__launch_bounds__(128, 8)` — estimated +66.7% kernel speedup

Research-0749 provides the hardware measurements on RTX 4090 (CC 8.9), ncu 2026.2.0,
CUDA 13.3.

## Decision

**Accept the `adm_cm_line_kernel_8` `__launch_bounds__` change; revert the
`ms_ssim_decimate` smem tiling.**

The adm_cm change reduces registers/thread from 114 to 64 (confirmed by ncu) and
delivers a measured -9.3% kernel duration improvement at 1080p. At 576x324 the wave
count bottleneck dominates regardless, and the change is noise-level neutral.

The ms_ssim_decimate smem tiling is a measured regression: kernel duration increases
+24% (WL1) and +8% (WL2). The baseline already achieves 95% L1 hit rate via hardware
prefetch — the tiling assumption (that DRAM bandwidth was the bottleneck) was invalid
for Ada Lovelace at both profiled resolutions. The cooperative-load pattern breaks the
hardware prefetcher and introduces a __syncthreads barrier with no net benefit.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep both changes | Positive end-to-end (+3.9–4.8%) driven by adm_cm | ms_ssim kernel regresses 8–24% hidden by adm_cm gain | Regressed kernel should not land |
| Revert both changes | Clean revert | Loses confirmed adm_cm improvement | adm_cm improvement is real and validated |
| Keep smem tiling, tune | Occupancy improves 3–8% | L1 hit rate drops 95%→24%; requires a fundamentally different load pattern | Not worth re-engineering in this PR |

## Consequences

- **Positive**: adm_cm register pressure reduced 43.9% (114→64 regs); measured -9.3% kernel duration at 1080p; correctness passes; no change to public API.
- **Negative**: ms_ssim_decimate smem tiling must be reverted; the estimated +68–93% improvement was an analysis error.
- **Neutral / follow-ups**: A correct optimization for ms_ssim_decimate at 1080p should explore occupancy via block-size tuning or horiz/vert kernel fusion (baseline occupancy is 79% at 1080p — the kernel is already throughput-limited, not memory-bound).

## References

- [Research-0749](../research/0749-cuda-ms-ssim-decimate-adm-cm-1080p-measure.md)
- Source: user direction per task spec dated 2026-05-28
