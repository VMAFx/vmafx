<!-- markdownlint-disable MD013 MD060 -->
# ADR-0024: Preserve Netflix source-of-truth tests verbatim

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: testing, ci, license

## Context

VMAF's numerical correctness is not arbitrary — it is defined by Netflix's reference test outputs. The fork adds SYCL/CUDA/SIMD paths that must match CPU golden scores. If golden assertions drift, cross-backend correctness claims become meaningless. User quote: "those 3 testpair files with scores by netflix... only the cpu ones... thats one normal and 2 checkerboard tests I believe".

## Decision

We will preserve Netflix's source-of-truth tests verbatim as the canonical ground-truth gate for VMAF numerical correctness (CPU only). The three Netflix reference test pairs are: (1) normal `src01_hrc00_576x324.yuv` ↔ `src01_hrc01_576x324.yuv`, (2) checkerboard A `checkerboard_1920_1080_10_3_0_0.yuv` ↔ `checkerboard_1920_1080_10_3_1_0.yuv` (1-px shift), (3) checkerboard B same ref ↔ `checkerboard_1920_1080_10_3_10_0.yuv` (10-px shift). Run in CI on the Linux x86_64 job as a required status check; not in pre-commit (too slow). Fork-added tests live in separate files/dirs; Netflix golden assertions are never modified. YUVs live in `python/test/resource/yuv/`; golden scores are hardcoded `assertAlmostEqual` assertions in `python/test/quality_runner_test.py`, `vmafexec_test.py`, `vmafexec_feature_extractor_test.py`, `feature_extractor_test.py`, `result_test.py`. No connection to `testdata/scores_cpu_*.json` (those are fork-added GPU/SIMD snapshots, not Netflix golden data).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Regenerate goldens from our CPU path | Easier to maintain | Loses Netflix as source of truth; makes the fork its own oracle | Defeats the whole correctness claim |
| Replace with synthetic fixtures | Flexible | Not validated against upstream | Same |
| Keep Netflix assertions, separate fork tests (chosen) | Preserves authoritative baseline; fork tests evolve freely | Two test locations | Rationale: Netflix is the upstream oracle |

This decision was a default — the alternative was unacceptable.

## Consequences

- **Positive**: any SIMD/GPU change that breaks CPU goldens is caught immediately; upstream sync does not have to reconcile assertion drift.
- **Negative**: fork-added tests must live in separate files to avoid accidental edits.
- **Neutral / follow-ups**: CLAUDE.md §12 rule 1 hard-codes "never modify Netflix golden assertions".

## References

- Source: `req` (user: "those 3 testpair files with scores by netflix... only the cpu ones... thats one normal and 2 checkerboard tests I believe")
- Related ADRs: ADR-0009 (snapshot regeneration), ADR-0037
