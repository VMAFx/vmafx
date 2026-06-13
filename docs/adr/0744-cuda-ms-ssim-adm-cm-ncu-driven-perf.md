<!-- markdownlint-disable MD013 MD050 MD060 -->
# ADR-0744: CUDA adm_cm `__launch_bounds__(128, 8)` register reduction (ms_ssim_decimate smem tiling reverted)

- **Status**: Accepted
- **Date**: 2026-05-28
- **Superseded by / Follow-up**: [ADR-0750](0750-cuda-ms-ssim-decimate-adm-cm-measure.md) (measurement)
- **Deciders**: lusoris
- **Tags**: cuda, performance, adm, ms_ssim, occupancy

## Context

PR #77 ncu profiles identified two CUDA kernels as bottlenecks:

1. `ms_ssim_decimate` — 81 global/L2 reads per output pixel, no shared-memory reuse, two `mirror_idx` calls per read (modulo + conditional branches).
2. `adm_cm_line_kernel_8` — 114 registers/thread, ~33% theoretical occupancy on Ampere (RTX 4090, CC 8.9).

Two optimisations were proposed and initially implemented (Research-0744):

- **Opt A** (`ms_ssim_decimate` smem tiling): cooperative CTA tile load of `TILE_H × TILE_W_PAD = 24 × 41` floats (3936 B), amortising `mirror_idx` cost to once per source element.
- **Opt B** (`adm_cm_line_kernel_8 __launch_bounds__(128, 8)`): instructs ptxas to target ≤64 regs/thread (65536 / (8 CTAs × 128 threads)), raising theoretical occupancy from 33% to ~67%.

Hardware measurement was conducted in PR #89 using `ncu 2026.2.0` on `vmaf-dev-mcp:cuda13.3` (RTX 4090, 128 SMs):

- **Opt A measured**: +8 to +24% kernel duration (regression at all tested resolutions — 576p and 1080p). Root cause: the baseline `ms_ssim_decimate` kernel was already L1-resident (95% hit rate measured by ncu MemoryWorkloadAnalysis). The cooperative-load + `__syncthreads()` overhead broke the hardware prefetcher and converted L1 hits into L2/DRAM misses.
- **Opt B measured**: −9.3% kernel duration at 1080p, neutral at 576p. Registers confirmed 114→64 per thread.

End-to-end aggregate: +3.9–4.8% fps improvement driven entirely by Opt B.

## Decision

- **Revert Opt A** (`ms_ssim_decimate` smem tiling). The L1-resident baseline makes smem staging a net regression. The non-tiled kernel remains at `core/src/feature/cuda/integer_ms_ssim/ms_ssim_score.cu`.
- **Keep Opt B** (`adm_cm_line_kernel_8 __launch_bounds__(128, 8)`). Confirmed −9.3% at 1080p; negligible at 576p (occupancy-limited only at larger grids). Change is in `core/src/feature/cuda/integer_adm/adm_cm.cu`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep both Opt A + Opt B | +Opt B gain retained | Opt A regresses ms_ssim by +8–24%; smem staging adds unnecessary complexity | Measured regression — PR #89 |
| Keep Opt A, drop Opt B | Possible gain if L1 occupancy drops on future hardware | Measured regression now; removes −9.3% adm_cm gain | Dominated by Opt B on current target arch |
| Drop both | No regression risk | Loses confirmed −9.3% adm_cm gain; end-to-end +3.9–4.8% fps lost | Leaves confirmed occupancy win on table |
| Keep Opt A with larger BLOCK size to reduce smem overhead | Might lower cooperative-load cost | L1 hit rate at baseline (95%) means problem is not DRAM throughput but prefetcher disruption; block-size change won't fix it | Root cause is prefetcher, not smem size |

## Consequences

- **Positive**: +3.9–4.8% fps end-to-end from adm_cm occupancy gain; ms_ssim_decimate performance unchanged (baseline was already L1-efficient).
- **Negative**: Smem tiling design is documented as a regression on current hardware (RTX 4090, CUDA 13.3). Future hardware with lower L1 hit rates may benefit — see Research-0744 and ADR-0750 for re-evaluation criteria.
- **Neutral**: Correctness unaffected. ADR-0214 places=4 parity gate confirmed bit-exact results before and after.

## References

- [Research-0744](../research/research-0744-cuda-ms-ssim-adm-cm-perf-impl.md) — analysis + implementation details
- [ADR-0750](0750-cuda-ms-ssim-decimate-adm-cm-measure.md) — hardware measurement (PR #89)
- [ADR-0454](0454-vif-cuda-smem-staging.md) — filter1d.cu smem precedent (L1-absent case)
- [ADR-0214](0214-gpu-parity-ci-gate.md) — GPU/CPU parity gate
- req: "Partial revert of PR #79 per the hardware measurement in PR #89 digest: revert ms_ssim_decimate smem tiling (measured -8 to -24%), keep adm_cm __launch_bounds__ (-9.3% at 1080p). Registers 114→64 confirmed."
