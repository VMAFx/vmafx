<!-- markdownlint-disable MD060 -->
# ADR-0544: deduplicate `feature_extractor_list[]` registrations

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (deep-audit follow-up)
- **Tags**: `bug`, `dispatch`, `vulkan`, `sycl`, `registry`

## Context

`core/src/feature/feature_extractor.c` exposes the runtime registry
through a single static array, `feature_extractor_list[]`.  Two iterator
clients walk that array on the hot path:

1. `vmaf_get_feature_extractor_by_name()` — first-match, returns on hit.
2. `vmaf_get_feature_extractor_by_feature_name()` — also first-match.

In addition, the ctx-pool's `get_fex_list_entry()` keys on `fex->name`
(string equality), so the same extractor symbol registered twice still
collapses to a single pool entry there.  But once a caller resolves an
extractor through the by-feature iterator and then walks the *registry*
itself — which the model-loader path does when expanding
`vmaf_use_features_from_model()` — every registered copy spawns its own
ctx-pool entry, and the pool drives `init`/`extract`/`flush` once per
entry per picture.

The deep audit on 2026-05-18 (`.workingdir/bbb_reports/DEEP_AUDIT_2026_05_18.md`,
Finding 22) found that:

- The `HAVE_VULKAN` block held **67 entries** instead of 18.
  `vmaf_fex_psnr_hvs_vulkan` and `vmaf_fex_float_ms_ssim_vulkan` were
  each registered **11 times**; seven other Vulkan twins were registered
  **6 times** each.
- The `HAVE_SYCL` block held **17 entries** instead of 11.  Six SYCL
  twins were registered **2 times** each (`psnr_sycl`,
  `float_moment_sycl`, `ciede_sycl`, `float_ssim_sycl`,
  `float_ms_ssim_sycl`, `psnr_hvs_sycl`).
- The `HAVE_CUDA`, `HAVE_HIP`, `HAVE_METAL`, and CPU blocks were clean.

The pattern of the duplicates (interleaved partial copies of the same
list) strongly suggests an editor mishap during one of the Vulkan
follow-up PRs that went undetected because the first-match `get_by_name`
path masked it.

The bug is a plausible root cause for the v9 CHUG "VMAF=99 across all
bitladders" anomaly that was previously attributed to operational
misuse in PR #1270 — Vulkan-twin extractors firing 6-11x per pic would
both inflate runtime and double-write the feature collector slots that
gate the final score.

## Decision

We will:

1. Edit `feature_extractor_list[]` so each `&vmaf_fex_*` symbol appears
   **exactly once** under its correct `#ifdef HAVE_*` guard.  Total
   reduction: **61 duplicate entries removed** (55 Vulkan + 6 SYCL).
2. Add `vmaf_feature_extractor_list_audit()` — an O(N²) one-shot
   walker that compares both pointer identity and `name` strings, logs
   each offender at `VMAF_LOG_LEVEL_ERROR`, and returns `-EINVAL` on
   any duplicate.
3. Call the audit from `vmaf_init()` before any other context state is
   created, so a regression fails fast at init time instead of silently
   doubling per-pic work.
4. Add a C unit test (`test_feature_extractor_list_no_duplicates` in
   `core/test/test_feature_extractor.c`) that exercises the audit
   helper on the live registry.  This wires the check into the
   `meson test` suite that already gates pushes.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Just delete the duplicate rows (no audit, no test) | Smallest diff | Nothing prevents a future editor mishap from re-introducing the bug; the original misregistration sat in the tree undetected for multiple releases | Insufficient guardrail given the cost of recurrence |
| Replace the array with a hash-set built at startup | Strongest dedup guarantee at runtime | Requires non-trivial allocator wiring + new init/teardown lifecycle; loses the constant-init guarantee of the static array | Disproportionate to the bug; the audit + test combo is sufficient |
| Use a generated `static_assert` instead of a runtime audit | Compile-time failure | C99 `_Static_assert` can't iterate a string-comparing predicate over a pointer array; would need a code-gen step | Adds build complexity for marginal gain over the runtime audit that already runs on every `vmaf_init` |
| Audit at every `get_by_name` call instead of once at init | Catches duplicates that creep in via dlopen-like extensions (we don't have any) | O(N²) work on every call when the cost only needs to be paid once | Init-time audit is the right scope |

## Consequences

- **Positive**: every iterator-driven dispatch path now allocates one
  ctx-pool entry per extractor instead of 2–11; per-pic
  init/extract/flush invocations drop by the same factor for the
  affected Vulkan / SYCL twins.  The audit catches the bug class at
  init time on every future build.
- **Negative**: `vmaf_init()` now performs an O(N²) walk (~3 600
  comparisons at N≈60).  Negligible — runs once per `VmafContext`.
- **Neutral / follow-ups**: the cross-backend numeric-parity sweep
  (`/cross-backend-diff`) is the right tool to confirm the Vulkan
  scores are now sane; the CHUG re-extract should be re-run against
  the deduped binary to verify the "VMAF=99" pattern is gone.  If
  Vulkan scores were genuinely meaningless because of the duplicate
  init/flush sequence, the snapshot JSONs under `testdata/` may need
  a `/regen-snapshots` pass — assess after the parity sweep.

## References

- Finding 22, `.workingdir/bbb_reports/DEEP_AUDIT_2026_05_18.md` (deep
  audit run; the raw count + symptom-to-root-cause mapping that
  triggered this fix).
- Companion CLAUDE.md §12 r12 (touched-file lint-clean rule) — the
  edited block also drops the trailing `// ...` C++-style comments in
  favour of `/* ... */` blocks for clang-format consistency.
- Companion CLAUDE.md §12 r13 (state.md update) — see
  `docs/state.md` "Recently closed" row referencing this ADR.
