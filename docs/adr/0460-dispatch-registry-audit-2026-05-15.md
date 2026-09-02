<!-- markdownlint-disable MD060 -->
# ADR-0460: Dispatch-strategy registry audit 2026-05-15

| Field | Value |
|-------|-------|
| Status | Accepted |
| Date | 2026-05-15 |
| Tags | dispatch, hip, metal, sycl, vulkan, correctness |

## Context

The dispatch-strategy layer consists of two separate surfaces:

1. `feature_extractor_list[]` in `core/src/feature/feature_extractor.c`
   — the linear table walked by `vmaf_get_feature_extractor_by_name` and
   `vmaf_get_feature_extractor_by_feature_name`.

2. `vmaf_<backend>_dispatch_supports()` in each backend's
   `dispatch_strategy.c` — a predicate callers use to test whether a
   named feature can route to that backend before binding GPU pictures.

An audit (see `docs/research/0135-dispatch-strategy-registry-audit-2026-05-15.md`)
found four defects:

- **SYCL**: 6 of 17 extractor pointers duplicated in
  `feature_extractor_list[]` (copy-paste during backend-parity PRs).
- **Vulkan**: same pattern, more severe — some entries appeared 11 times.
- **HIP**: `vmaf_hip_dispatch_supports()` contained only a `TODO` stub
  returning 0 unconditionally, even though 8 kernels were registered.
- **Metal**: `g_metal_features[]` in `vmaf_metal_dispatch_supports()`
  used placeholder/approximate names that did not match the actual
  `provided_features[]` arrays in the `.mm` extractor files.

## Decision

Fix all four defects mechanically:

- Deduplicate SYCL and Vulkan sections of `feature_extractor_list[]`.
- Replace the HIP stub with a `g_hip_features[]` table enumerating all
  8 registered HIP extractor `.name` strings.
- Correct the Metal `g_metal_features[]` table to match each extractor's
  actual `.name` and `provided_features[]` values.

Add `scripts/ci/check-dispatch-registry.sh` to catch regressions, and
add invariant notes to `core/src/hip/AGENTS.md` and
`core/src/metal/AGENTS.md`.

## Alternatives considered

- **No action / document only**: rejected — the HIP predicate returning
  0 for all registered kernels is a functional bug (callers that check
  `dispatch_supports()` before routing will never select HIP); the Metal
  wrong names are likewise functional bugs.
- **ADR per backend**: unnecessary — all four fixes are mechanical and
  driven entirely by the extractor source of truth (`.name` /
  `provided_features[]`). No novel routing decision is required.

## Consequences

- `vmaf_hip_dispatch_supports()` now returns 1 for all 8 HIP extractor
  names; callers routing via this predicate will now correctly select HIP.
- `vmaf_metal_dispatch_supports()` now returns 1 for the correct set of
  feature names; callers routing via this predicate will now correctly
  select Metal for the 8 shipped extractors.
- The SYCL/Vulkan deduplication is cosmetic (first-match semantics meant
  behaviour was unchanged) but reduces table noise.
- `scripts/ci/check-dispatch-registry.sh` can be integrated into CI to
  prevent future divergence.

## References

- Research digest: `docs/research/0135-dispatch-strategy-registry-audit-2026-05-15.md`
- Per user direction: dispatch-strategy registry audit task (2026-05-15)
