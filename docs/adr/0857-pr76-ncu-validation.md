# ADR-0857: PR #76 filter1d NCU A/B measurement — accept optimization for production

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `cuda`, `perf`

## Context

PR #76 (`perf/cuda-vif-filter1d-ncu-driven-20260528`) added `__launch_bounds__(128, 10)`
and `__ldg()` to `filter1d_8_horizontal_kernel_2_17_9` as specified by ADR-0743.
The PR was submitted as DRAFT pending live NCU measurement at 1080p (the original
ADR-0743 measurement was at 576p, a wave-limited regime). The handoff noted that no
1080p A/B data had been formally published. This ADR records the measurement decision:
run live NCU A/B at 576p and 1080p, compare results, and decide whether to mark PR #76
ready for production merge.

## Decision

The optimization is confirmed production-ready based on live NCU measurements on RTX 4090
sm_89 (CUDA 13.3, ncu 2026.1.1.0) at both 576p and 1080p. PR #76 is accepted for merge.
See Research-0857 (`docs/research/research-0857-pr76-ncu-validation-20260529.md`) for
full numeric data.

Key measurements (1080p, 3-frame checkerboard workload):
- `launch__registers_per_thread`: 56 → 48 (−14.3%)
- `sm__warps_active`: 66.4% → 72.4% (+6.0 pp)
- `l1tex__t_bytes.sum`: 75.9 MB → 117.3 MB (+54.7%, `__ldg` confirmed)
- E2E fps: 1980 → 2016 (+1.8%, directionally positive)
- Correctness: bit-identical (delta = 0.000000)

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Wait for 4K measurement | More complete picture | No 4K YUV available; 4K is already confirmed fully saturated per Research-0751 (253 waves, 69.7% active warps) | Deferred — Research-0751 already covers 4K |
| Block merge until 48f+ workload at 1080p | Lower fps variance | Delays merge; directional result already clear | Not needed for production decision |
| Revert optimization | Eliminates risk | Throws away confirmed +6 pp warp activity improvement | No regression found; revert unwarranted |

## Consequences

- PR #76 can be marked READY and merged to master.
- Filter1d 1080p fps baseline established: 2016 fps (optimized), 1980 fps (pre-opt).
- Future filter1d changes measured against this baseline.
- `testdata/perf_benchmark_results.json` does not contain a 1080p CUDA filter1d entry;
  this ADR establishes the informal baseline until a formal bench run adds one.

## References

- ADR-0743: original optimization decision (`docs/adr/0743-cuda-vif-filter1d-ncu-driven-perf.md`)
- Research-0857: live measurement data (`docs/research/research-0857-pr76-ncu-validation-20260529.md`)
- Research-0748: earlier 1080p measurement from the PR branch (on `origin/research/cuda-vif-filter1d-1080p-remeasure-20260528`, not yet merged)
- Research-0751: 4K saturation data confirming filter1d is wave-rich at 4K
