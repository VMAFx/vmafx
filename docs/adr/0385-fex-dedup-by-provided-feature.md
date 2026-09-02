<!-- markdownlint-disable MD013 MD060 -->
# ADR-0385: Feature-extractor deduplication by provided-feature names

- **Status**: Accepted
- **Date**: 2026-05-10
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `correctness`, `cuda`, `gpu`, `feature-extractor`, `fork-local`

## Context

`feature_extractor_vector_append()` in `core/src/fex_ctx_vector.c`
is the single registration point for all feature extractors. Before
each frame is scored, the registered vector is iterated and every
extractor's `extract()` callback writes its results into the shared
`VmafFeatureCollector`. That collector enforces a strict
one-write-per-feature-per-frame rule.

When the CLI combines `--feature <name>` with an auto-loaded VMAF
model (i.e., when `--no_prediction` is absent), both
`vmaf_use_features_from_model()` and the explicit `--feature` handler
call `feature_extractor_vector_append()`. On a CUDA-enabled binary
with an active CUDA device, `vmaf_use_features_from_model()` resolves
the CUDA twin (e.g. `adm_cuda`) while the `--feature adm` path
resolves the CPU extractor `adm`. Both are appended, both run at every
frame, and both write the same collector slots — producing hundreds of
"cannot be overwritten" warnings per scoring run and silently
discarding one backend's results.

The prior dedup key was derived from `vmaf_feature_name_from_options(fex->name, ...)`,
which encodes the extractor's own name (`"adm"` vs `"adm_cuda"`).
These strings differ, so the dedup check passed and both extractors
were registered. The function was designed for option-keyed caching
within a single backend, not for cross-backend twin detection.

## Decision

Change the dedup key in `feature_extractor_vector_append()` from the
extractor name to the set of provided-feature names. A new
`provided_features_overlap()` helper returns true when any string from
`fex->provided_features[]` matches any string from the already-
registered extractor's `provided_features[]`. If overlap is detected,
the incoming context is destroyed and registration is skipped; a
DEBUG-level log records which extractor was skipped and which
registered extractor it overlapped with. No WARNING is emitted — the
dedup is the correct outcome.

A legacy fallback retains the original name-based comparison for
extractors with a NULL `provided_features` pointer, preserving
compatibility with any future extractor that omits the declaration.

This approach is correct because the cross-backend parity contract
(ADR-0214) requires every CPU/GPU twin to emit the same logical
feature set under the same names. Any extractor pair that would
produce a "cannot be overwritten" collision is therefore also a
`provided_features` overlap by contract.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **A — Add `--no_prediction` to the CLI when `--feature` is given** | Simple; no engine change | Changes user-visible semantics: prediction (VMAF score) would be suppressed whenever `--feature` is used, surprising users who want both a custom extractor and the full VMAF score | Rejected — wrong semantics |
| **B — Dedup by provided-feature names (chosen)** | Catches all CPU/GPU/SYCL/Vulkan/HIP twins; zero code change in callers; correct for the full feature-twin matrix; satisfies ADR-0214 parity contract | Slightly more expensive dedup loop (O(m×n) over provided_features arrays vs O(1) string compare on name); in practice, the registration phase runs once per session, not per frame | Chosen |
| **C — Dedup by extractor-name prefix strip** | Remove `_cuda` / `_sycl` / `_vulkan` / `_hip` / `_metal` suffix, compare base name | Fragile: suffix list must be maintained; breaks for custom extractors with coincidentally similar names; does not catch option-parametrised twins | Rejected — brittle |
| **D — Prevent `vmaf_use_features_from_model` from registering an extractor already registered by `vmaf_use_feature`** | Dedup at the caller level rather than the vector level | Requires each caller to inspect the already-registered vector before calling append; spreads the dedup policy across multiple call sites | Rejected — wrong layer |

## Consequences

- **Positive**: Zero "cannot be overwritten" warnings when combining
  `--feature <name>` with a default VMAF model load on any GPU binary.
  The first-registered extractor (from `vmaf_use_features_from_model`,
  which selects the active backend twin) wins; the CPU extractor added
  by `--feature` is silently dropped, which is the correct priority.
- **Positive**: The fix is transparent to all callers — no change to
  `vmaf_use_features_from_model`, `vmaf_use_feature`, or any extractor
  registration code outside the vector.
- **Positive**: Covers all current and future backends (CUDA, SYCL,
  Vulkan, HIP, Metal) without per-backend special-casing.
- **Negative**: The dedup loop is now O(m×n) where m and n are the
  sizes of the two `provided_features` arrays (typically 5–15 entries
  each). Registration runs once per session, not per frame, so the
  overhead is negligible.
- **Neutral**: The `feature_extractor_vector_append()` signature and
  return contract are unchanged. Callers that rely on the function
  returning 0 on both "appended" and "deduped" paths continue to work
  correctly.

## References

- [Research-0096](../research/0096-fex-dedup-by-provided-feature-2026-05-10.md) — root-cause analysis and verification.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — cross-backend parity contract (same provided-feature names per twin).
- Bug surfaced in: PR #739 (K150K redesign agent, merged 2026-05-10); fix deferred to this PR.
- `docs/state.md` row: T-CUDA-FEATURE-EXTRACTOR-DOUBLE-WRITE (closed by this PR).
- req: "Fix T-CUDA-FEATURE-EXTRACTOR-DOUBLE-WRITE ... make `feature_extractor_vector_append()` deduplicate by provided-feature names ... catches CPU/CUDA/SYCL/Vulkan/HIP/Metal twins as duplicates because they all advertise the same provided-feature names per ADR-0214."
