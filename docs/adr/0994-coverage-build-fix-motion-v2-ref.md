# ADR-0994: Fix Coverage Gate build break — `integer_motion.c` compile error

- **Status**: Accepted
- **Date**: 2026-06-03
- **Deciders**: Lusoris
- **Tags**: `ci`, `coverage`, `build`, `motion`

## Context

The Coverage Gate (ADR-0922) has been failing on every master push since
commit `c658b3c452` with a build error rather than a coverage threshold
failure:

```text
integer_motion.c:324:49: error: 'VmafFeatureExtractor' has no member
named 'prev_prev_ref'; did you mean 'prev_ref'?
```

This caused the entire coverage job to abort before `gcovr` ran, so no
coverage report was produced. Two related bugs existed simultaneously:

1. **`integer_motion.c` compile error**: `extract()` referenced
   `fex->prev_prev_ref`, a field that does not exist on
   `VmafFeatureExtractor`. The struct only has `prev_ref`
   (one-frame-back). The `motion_five_frame_window` feature requires
   a two-frames-back reference not yet plumbed through the framework
   (deferred per ADR-0337). `integer_motion_v2.c` already handled this
   correctly by rejecting `motion_five_frame_window=true` in `init()`
   with `-ENOTSUP`.

2. **`feature_extractor.cpp` linker error**: `feature_extractor.cpp`
   still declared `extern VmafFeatureExtractor vmaf_fex_integer_motion_v2`
   and referenced it in `feature_extractor_list[]`, even though the
   corresponding `.c` source was removed from the meson build.
   `feature_extractor.c` had already been updated to remove this
   declaration, but `feature_extractor.cpp` (the file compiled by
   meson) had not.

## Decision

Fix both issues in a single targeted patch:

1. In `integer_motion.c`: add `log.h` include and an `-ENOTSUP` guard
   in `init()` when `motion_five_frame_window=true`, mirroring the
   pattern in `integer_motion_v2.c`. Replace the dead
   `&fex->prev_prev_ref` reference in `extract()` with `&fex->prev_ref`.
   Add a comment documenting the deferral and reversibility.

2. In `feature_extractor.cpp`: remove the
   `extern VmafFeatureExtractor vmaf_fex_integer_motion_v2`
   declaration and its entry in `feature_extractor_list[]`, matching
   `feature_extractor.c`. Add the same comment citing the upstream
   merge.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Add `prev_prev_ref` to struct | Unblocks 5-frame mode | Requires picture-pool refactor (ADR-0337, ADR-0152) — multi-PR effort | Deferred per ADR-0337; wrong scope |
| Add `integer_motion_v2.c` to meson | Restores CPU motion_v2 symbol | Upstream merge intentionally removed it; reverting diverges | Diverges from upstream |
| Lower coverage floor | Unblocks CI | Hides the build break | Against ADR-0637 / ADR-0922 |

## Consequences

- **Positive**: the Coverage Gate can now execute and report an actual
  coverage percentage instead of aborting at the build step. The gate
  was silently broken for all master pushes since `c658b3c452`.
- **Negative**: `motion_five_frame_window=true` now returns `-ENOTSUP`
  on both `motion` and `motion_v2` (the v2 restriction was already in
  place).
- **Neutral / follow-ups**: the `prev_prev_ref` plumbing work
  (ADR-0337) remains deferred. When that PR lands, both extractors
  must flip their `-ENOTSUP` guards to real `prev_prev_ref` lookups
  per the rebase-notes ledger.

## References

- ADR-0337: motion_v2 public option surface — defers `prev_prev_ref`.
- ADR-0922: coverage ratchet / 70% floor.
- ADR-0152: `vmaf_read_pictures` monotonic-index decomposition.
- CI failure: `c658b3c452` — Coverage Gate broken on every push since.
