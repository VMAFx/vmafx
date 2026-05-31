<!-- markdownlint-disable MD013 MD060 -->
<!--
  Copyright 2026 Lusoris
  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
-->
# ADR-0760: CUDA motion kernel multi-resolution ncu profiling methodology

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `cuda`, `perf`, `research`

## Context

The CUDA motion feature is the only VMAF feature where GPU throughput is
lower than CPU at 4K (Research-0751: CPU 290.8 fps vs CUDA 175.5 fps at
4K). Prior ncu analysis (Research-0735) profiled 576p only. Without a
multi-resolution profile, the bottleneck class and the resolution at which
the transition from dispatch-bound to compute-bound occurs are unknown.

This ADR documents the decision to run the multi-resolution profiling
analysis before proposing any implementation changes, and records the
approach taken (ncu `--set basic` with container isolation, three
resolutions, live kernel metrics + wall-time cross-check).

## Decision

Profile `calculate_motion_score_kernel_8bpc` at 576p, 1080p, and 4K using
ncu `--set basic` in a one-off `vmaf-dev-mcp:cuda13.3` container before
any optimization is implemented. Capture kernel duration, occupancy, DRAM
throughput, compute SM throughput, waves/SM, and registers per thread.
Cross-check with end-to-end wall-time measurements to separate kernel time
from dispatch overhead.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Profile at 4K only | Minimal profiling time | Misses the dispatch-dominated regime that defines the CUDA/CPU ratio at sub-4K | Sub-4K is the primary deployment target |
| Use `perf record` instead of ncu | Host-native, no PMU privilege | Does not break down CUDA kernel vs dispatch overhead | GPU-specific data required |
| Implement batching first, then profile | Fast path to improvement | Risks implementing the wrong fix if bottleneck class is wrong | Profile first per spec |

## Consequences

- **Positive**: The multi-resolution data (Research-0760) confirms the
  bottleneck is dispatch overhead at 576p/1080p and latency-bound compute
  at 4K. Optimizations can now be ordered correctly: batching first (largest
  leverage), then separable filter (4K improvement).
- **Negative**: Research-only commit; no user-visible improvement yet.

## References

- req: "Profile motion CUDA kernel at 576p, 1080p, and 4K to identify optimization candidates"
- Research-0760 (`docs/research/0760-cuda-motion-ncu-multi-resolution-20260529.md`)
- Research-0735 (prior 576p-only analysis)
- Research-0751 (PR #90 4K cross-backend baseline)
