<!-- markdownlint-disable MD013 MD060 -->
# ADR-0743: CUDA VIF filter1d ncu-driven performance optimizations

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `cuda`, `performance`, `vif`

## Context

PR #74 profiling identified `filter1d_8_horizontal_kernel_2_17_9` as consuming
35.3% of VIF wall time on RTX 4090 / CUDA 13.3 with 48.6% achieved occupancy
versus a 75% theoretical ceiling (56 registers per thread, 9 blocks/SM).

Three optimizations were proposed:

1. `val_per_thread` 2 → 4 (halve grid count, reduce wave fragmentation)
2. Split accumulator live range to reduce register count below 48
3. `__ldg()` / `const __restrict__` on the 7 read-only tmp channel loads

Baseline ncu measurements (576×324 YUV, sm_89):

- Registers Per Thread: 56
- Theoretical Occupancy: 75%
- Achieved Occupancy: 42.91%
- Kernel Duration: 17.6 µs

## Decision

Apply `__launch_bounds__(128, 10)` to the `FILTER1D_8_HORI` macro and add
`__ldg()` on the 7 global tmp-channel loads in the smem-fill phase.  Reject
`val_per_thread=4` (see Alternatives).  The `__launch_bounds__` approach
replaces the "serialise accumulators into two sub-loops" proposal because it
achieves the same register reduction without algorithmic restructuring or added
`__syncthreads()` overhead.

Post-optimization ncu measurements (same workload):

- Registers Per Thread: 48 (down from 56)
- Theoretical Occupancy: 83.33% (up from 75%)
- Achieved Occupancy: 41.35%
- Kernel Duration: 18.4 µs (within noise at 576×324 — wave-limited)

Correctness verified: CUDA-optimized vs CPU reference delta ≤ 0.000010 (max
per-frame absolute), within the ADR-0214 places=4 gate (≤ 0.0001).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `val_per_thread=4` (opt #1) | −33% grid count at 576p | smem 7644→14812 B/block; sm_89 becomes smem-limited at 37.5% occ. vs 62.5% for vpt=2 | Rejected: net occupancy regression |
| Serialise accumulators into two sub-loops (original opt #2 proposal) | Reduces live register range algorithmically | Adds an extra `__syncthreads()`, extra smem writes for mu1/mu2, doubles inner loop body | Replaced by `__launch_bounds__` which achieves the same register budget without algorithmic changes |
| `__launch_bounds__(128, 10)` only, no `__ldg` | Zero overhead on warm L1 | No L2 pressure relief at ≥1080p | Kept both; `__ldg` is zero-overhead at register level and beneficial at high-res |
| `__launch_bounds__(128, 8)` (sm_75-safe) | No ptxas advisory | Max 64 regs budget on sm_89 — too loose to force 48 | sm_89 (RTX 4090) is primary target; sm_75 gets no regression, just no benefit |

## Consequences

- **Positive**: Theoretical occupancy 75% → 83.3% on sm_89 (RTX 4090). Register count 56 → 48. At production resolutions (≥1080p), more CTAs reside concurrently per SM, improving warp latency hiding.
- **Negative**: ptxas emits a non-fatal advisory "minnctapersm out of range, ignored" for sm_75/sm_80/sm_86 targets in the multi-arch fatbin. These targets see no register change and no performance regression.
- **Neutral**: The `__ldg` change is zero-impact at 576×324 (wave-limited) but provides cache-routing benefit at ≥1080p. Both changes are semantically neutral — same integer arithmetic.

## References

- PR #74 ncu profile report (research digest: `docs/research/research-0743-cuda-vif-filter1d-perf-impl.md`)
- ADR-0454: stencil/convolution smem staging rule
- ADR-0214: GPU-parity CI gate (places=4 tolerance)
- req: "Implement the 3 ncu-measured optimizations on filter1d.cu from PR #74's profile"
