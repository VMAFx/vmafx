# ADR-0739: CompileIQ Pilot on `filter1d.cu` — Abandoned (Toolchain Blockers)

- **Status**: Accepted (pilot abandoned; re-run pre-conditions documented)
- **Date**: 2026-05-28
- **Deciders**: Lusoris
- **Tags**: `cuda`, `build`, `perf`, `ci`

## Context

The user requested a pilot of NVIDIA CompileIQ v1.0.0 (a hyperparameter optimizer
for NVIDIA compiler controls) on `core/src/feature/cuda/integer_vif/filter1d.cu`
(845 LOC). The goal was to auto-tune PTXAS/NVCC compiler controls, persist the
resulting ACF file in-tree under `-Denable_compileiq=true`, and ship the change if
perf improved ≥3% with zero numeric divergence.

Per CLAUDE.md §15, all CUDA work must run inside the `vmaf-dev-mcp` container (or a
one-off instance of its image). The pilot was therefore scoped entirely to the
container toolchain.

Two hard blockers were identified before the search ran (see Research-0734).

## Decision

We will not merge any CompileIQ-generated ACF file at this time. The pilot is
documented as abandoned pending:

1. Container Python downgrade to ≤3.13 (CompileIQ 1.0.0 requires `<3.14`).
2. Container CUDA toolkit upgrade to 13.3 (CompileIQ's only published search space).

The ADR is accepted to record the decision and pre-conditions so a future agent does
not re-discover these blockers from scratch.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Run CompileIQ on host (Python 3.11.15 available) | Unblocks the pilot immediately | Violates CLAUDE.md §15 for CUDA work; results may not transfer to container builds | Rejected — rules violation without explicit user sign-off |
| Patch Containerfile to add Python 3.12 alongside 3.14 | Restores compatibility, no base-image change | Container builds are slow; CUDA 13.2 search space still missing | Not worth unblocking half the problem; wait until both are resolved |
| Vendor ACF from host-side CompileIQ run | Sidesteps search-space version gap | ACF is toolkit-version-specific; an ACF from 13.3 catalog applied to 13.2 nvcc is undefined behavior | Rejected |
| Wait for CompileIQ to publish Python 3.14 wheel | No action required now | Timeline unknown; NVIDIA filed the `<3.14` cap deliberately | Deferred — no ETA |

## Consequences

- **Positive**: Blockers are documented with exact error evidence, preventing future NO-OP re-investigations.
- **Negative**: No perf improvement landed. `filter1d.cu` remains un-tuned.
- **Neutral / follow-ups**:
  - Open tracking items: (a) container Python ≤3.13, (b) container CUDA ≥13.3.
  - Re-run checklist: Python version gate + `nvcc --version | grep -E "13\.[3-9]"` before dispatching.
  - If host-only path is approved explicitly by the user, the objective function scaffold
    (`scripts/cuda/compileiq_filter1d_objective.py`) can be authored at that time.

## References

- Research digest: [docs/research/0734-cuda-compileiq-filter1d-pilot.md](../research/0734-cuda-compileiq-filter1d-pilot.md)
- CompileIQ PyPI: <https://pypi.org/project/compileiq/>
- CompileIQ GitHub: <https://github.com/NVIDIA/CompileIQ>
- Target: `core/src/feature/cuda/integer_vif/filter1d.cu`
- req: user instruction 2026-05-28 — "Pilot NVIDIA's CompileIQ on filter1d.cu (845 LOC)"
