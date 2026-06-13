# ADR-0780: NOLINT Cluster Refactor — Slab Allocator, SYCL Stride, ADM Band-Size

<!-- markdownlint-disable MD013 -- ADR/research body text; pre-existing long lines per ADR-0864 tail -->

- **Status**: Proposed
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `ci`, `simd`, `cuda`, `sycl`, `hip`, `lint`

## Context

A sweep of the 218 `NOLINT` annotations under `core/src/` (PR #61, ADR-0278 citation
closeout) identified five clusters where more than five suppressions of the same
category appear in a single file or tightly related file group. Three of these clusters
are refactorable: (1) bare `performance-no-int-to-ptr` annotations in GPU slab
allocators violate ADR-0278 (no citation) and could be eliminated by a shared macro;
(2) `bugprone-implicit-widening-of-multiplication-result` in SYCL stride arithmetic
could be eliminated entirely by adding explicit `(ptrdiff_t)` casts; (3) bare
`readability-function-size` annotations in `integer_adm.c` also lack citations and
could be consolidated into NOLINTBEGIN/NOLINTEND blocks. The remaining two clusters
(SYCL `misc-const-correctness` and SYCL `readability-function-size`) are either
consolidation opportunities or load-bearing per ADR-0141.

The user requested a cluster digest and a plan for follow-up refactor PRs; this ADR
records the decision to proceed with the three-PR sequence described in the research
digest (`docs/research/nolint-cluster-audit-2026-05-29.md`).

## Decision

We will execute three independent follow-up PRs:

- **PR A** (`sycl-stride-cast`): Replace implicit-widening SYCL stride multiplications
  with explicit `(ptrdiff_t)` casts in `integer_adm_sycl.cpp` and
  `integer_vif_sycl.cpp`, eliminating 12 suppressions entirely.
- **PR B** (`gpu-slab-macro`): Introduce `core/src/feature/gpu_slab.h` with a
  `SLAB_FIELD(ptr, type)` macro; migrate `hip/integer_vif_hip.c`,
  `cuda/integer_vif_cuda.c`, and `cuda/integer_adm_cuda.c`, eliminating 21 bare
  uncited annotations.
- **PR C** (`adm-nolint-consolidation`): Extend SYCL `NOLINTBEGIN` blocks to include
  `misc-const-correctness`; add a `NOLINTBEGIN(readability-function-size)` block with
  ADR-0141/ADR-0278 citations in `integer_adm.c`, eliminating 27 inline annotations.

The SYCL `readability-function-size` cluster (11 annotations) is load-bearing per
ADR-0141 §2 and is not touched.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Add citations only (no structural change) | Minimal diff | Leaves 47 annotations in place; doesn't reduce visual noise | Less clean than eliminating or consolidating |
| Single mega-PR with all changes | One review round | Three unrelated files; harder to bisect if one change regresses | Independent PRs allow staged CI |
| Suppress at `.clang-tidy` project level | Smallest diff | Hides legitimate future violations | Too broad; defeats the per-site rationale |

## Consequences

- **Positive**: 47 of 71 clustered annotations eliminated or consolidated; `gpu_slab.h`
  establishes a reusable pattern for future GPU backends; SYCL files become easier to
  read without repeated identical suppressions.
- **Negative**: `gpu_slab.h` is a new dependency for GPU feature files; must be kept
  in sync with any future slab layout changes.
- **Neutral / follow-ups**: PRs A–C can be dispatched in parallel worktrees. Each PR
  is lint-clean and bit-exact (no semantic change to the C/SYCL arithmetic).

## References

- Research digest: `docs/research/nolint-cluster-audit-2026-05-29.md`
- ADR-0141 (touched-file cleanup + load-bearing NOLINT rule)
- ADR-0278 (NOLINT citation closeout)
- Related: `core/src/feature/sycl/integer_vif_sycl.cpp`,
  `core/src/feature/sycl/integer_adm_sycl.cpp`,
  `core/src/feature/hip/integer_vif_hip.c`,
  `core/src/feature/integer_adm.c`
- Source: user direction (paraphrased): sweep for NOLINT clusters larger than 5 in
  the same area; assess root cause, refactor feasibility, and ADR-0278 compliance;
  produce digest and open a ready PR.
