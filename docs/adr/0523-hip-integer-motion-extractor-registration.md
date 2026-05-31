<!-- markdownlint-disable MD013 MD060 -->
# ADR-0523: Register `vmaf_fex_integer_motion_hip` in the extractor list

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `hip`, `gpu`, `feature-extractor`, `bugfix`, `fork-local`

## Context

`core/src/feature/hip/integer_motion_hip.c` defines
`vmaf_fex_integer_motion_hip` (extractor name `"motion_hip"`, emitting
`VMAF_integer_feature_motion_score`, `_motion2_score`, and `_motion3_score`).
The source file's own comment at line 663 states the extractor is
"registered via `extern VmafFeatureExtractor vmaf_fex_integer_motion_hip;` in
`feature_extractor.c`'s `feature_extractor_list[]`" — but neither the `extern`
declaration nor the list entry were ever added.

As a result, `vmaf_get_feature_extractor_by_name("motion_hip")` returned NULL
on every build, and `vmaf_use_feature(vmaf, "motion_hip", NULL)` returned a
non-zero error code. The parity test `test_hip_motion3_parity` (added in
PR #1167) hit this path and silently reported failure at the
`vmaf_use_feature` call, making the test meaningless since it was added.

The bug was surfaced by the HIP import-state agent (PR #1279 / ADR-0519),
which triggered a broader audit of HIP extractor registrations.

## Decision

Add the missing `extern VmafFeatureExtractor vmaf_fex_integer_motion_hip;`
declaration inside the `#if HAVE_HIP` block in
`core/src/feature/feature_extractor.c`, and register
`&vmaf_fex_integer_motion_hip` in `feature_extractor_list[]` (also inside
`#if HAVE_HIP`) immediately after the `vmaf_fex_integer_motion_v2_hip` entry,
mirroring the CUDA twin's position relative to `vmaf_fex_integer_motion_v2_cuda`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add declaration + registration (chosen) | Fixes the name lookup; makes `test_hip_motion3_parity` exercise real code | None | — |
| Remove `test_hip_motion3_parity` | Smaller diff | Deletes a correctness gate that has value once the real kernel lands | Rejected — never weaken tests |
| Move the extractor definition to a different file | Would allow the current linker order to work | Unnecessary churn; the definition site is correct | Rejected |

## Consequences

- **Positive**: `vmaf_get_feature_extractor_by_name("motion_hip")` now
  returns a non-NULL pointer on any build with `HAVE_HIP`. The lookup
  path, flag inspection, and `init()` invocation path are all exercised;
  `init()` still returns `-ENOSYS` in scaffold builds (unchanged posture).
  `test_hip_motion3_parity` now reaches the extractor-lookup assertion
  rather than failing at `vmaf_use_feature`.
- **Negative**: None. The extractor definition already compiles into the
  binary; the only change is wiring the name lookup.
- **Neutral / follow-ups**: No rebase impact on upstream paths. The CUDA
  twin (`vmaf_fex_integer_motion_cuda`) and SYCL twin
  (`vmaf_fex_integer_motion_sycl`) registrations remain unchanged.
  When the T7-10b real-kernel PR promotes `integer_motion_hip` from
  scaffold to real, no further change to `feature_extractor.c` is needed.

## References

- Source file: `core/src/feature/hip/integer_motion_hip.c` (lines 663–694)
- Broken test: `core/test/test_hip_motion3_parity.c` (line 162)
- Surfaced by: PR #1279 / [ADR-0519](0519-hip-import-state-implementation.md) HIP import-state audit
- CUDA twin registration: `core/src/feature/feature_extractor.c` line 257 (`vmaf_fex_integer_motion_cuda`)
- `req`: "fix the latent dead-code — `vmaf_fex_integer_motion_hip` is declared but never registered"
