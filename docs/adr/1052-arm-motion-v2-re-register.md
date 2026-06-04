<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1052: Re-register CPU `motion_v2` extractor and fix post-flush test ordering

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `core`, `motion`, `test`, `build`

## Context

PR #532 (commit 6bb5464511) ported upstream Netflix/vmaf commit a4a1492d3, which
restructured the motion extractor family. During the port, `vmaf_fex_integer_motion_v2`
(the CPU pipelined-SAD variant registered under the name `"motion_v2"`) was removed from
`feature_extractor.c`'s extern declaration list and from `feature_extractor_list[]`. The
source file `integer_motion_v2.c` still compiles on GPU-backend builds (it provides the
CUDA/SYCL/HIP/Metal `motion_v2` CPU fallback), but in a CPU-only build the symbol is
unreachable via `vmaf_get_feature_extractor_by_name("motion_v2")`, causing:

1. `test_motion_v2_missing` — every test that calls `vmaf_get_feature_extractor_by_name("motion_v2")` receives NULL and fails the `mu_assert("motion_v2 extractor missing", fex != NULL)` assertion.
2. `test_motion_three_frame` — `test_motion_three_frame_extract_emits_scores` called `vmaf_feature_collector_get_score` for `VMAF_integer_feature_motion2_score` *before* `vmaf_feature_extractor_context_flush()`. After the pipelined-rename port, `motion2_score` is deferred to `flush()`, so the pre-flush `get_score` always returns an error.

Both failures surface on ARM64 CI lanes (`Build — Ubuntu ARM clang (CPU)`) because those
lanes use CPU-only builds where the missing registration is fatal.

## Decision

Re-register the CPU `motion_v2` extractor in three steps:

1. Add `feature_src_dir + 'integer_motion_v2.c'` to the CPU source list in `core/src/meson.build` immediately after `integer_motion.c`.
2. Restore `extern VmafFeatureExtractor vmaf_fex_integer_motion_v2;` in `core/src/feature/feature_extractor.c` and add `&vmaf_fex_integer_motion_v2` to `feature_extractor_list[]` alongside `&vmaf_fex_integer_motion`.
3. Fix `test_motion_three_frame_extract_emits_scores` to call `vmaf_feature_collector_get_score` for `motion2_score` *after* `vmaf_feature_extractor_context_flush()`, matching the post-port contract.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Delete the failing tests | No build change needed | Loses coverage of the pipelined motion path; violates the "re-register is preferable to deleting the test" direction | Not chosen per task constraint |
| Keep motion_v2 only in GPU builds | Reduces CPU binary size slightly | Breaks any caller that requests `"motion_v2"` on a CPU-only build; breaks the existing coverage tests | Not chosen |

## Consequences

- **Positive**: `vmaf_get_feature_extractor_by_name("motion_v2")` returns a valid extractor on all build configurations; all `motion_v2` coverage tests pass on ARM64 CPU-only CI.
- **Negative**: `integer_motion_v2.c` is now compiled into the CPU library again (adds ~5 KB of object code).
- **Neutral**: The `motion_v2_score` deferred-to-flush contract is now documented in the test comment for future contributors.

## References

- Introducing PR #532 / commit `6bb5464511` (upstream port of Netflix/vmaf@a4a1492d3).
- Bisect root-cause reports: `test_motion_v2_missing` and `test_motion_three_frame`.
- Related: ADR-0337 (motion_five_frame_window -ENOTSUP), ADR-0994 (coverage-gate build break).
