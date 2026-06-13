<!-- markdownlint-disable MD060 -->
# ADR-0132: Port Netflix#1406 — `feature_collector` mount/unmount model-list bugfix

- **Status**: Accepted
- **Date**: 2026-04-20
- **Deciders**: @lusoris, Claude
- **Tags**: upstream-port, correctness, testing

## Context

Upstream PR [Netflix#1406][pr1406] (open, 1 commit, +117/-33) fixes two
bugs in the feature-collector's singly-linked model list under
`core/src/feature/feature_collector.c`:

1. `vmaf_feature_collector_mount_model()` advances `*head`
   (dereferencing the pointer-to-pointer and assigning back) instead
   of walking a local traversal cursor. Result: mounting ≥ 3 models
   corrupts the list — the head element is overwritten with its own
   successor, and every call past the first two loses earlier
   entries.
2. `vmaf_feature_collector_unmount_model()` returns `-EINVAL` on
   not-found instead of a semantically correct "not present"
   indicator. Callers cannot distinguish a programmer error (passing
   NULL) from a legitimate "this model isn't mounted".

The fork inherited both bugs verbatim and had test coverage for only
the trivial one-model case, so the regression was latent. The fork's
T4-4 row in [`BACKLOG.md`](../../.workingdir2/BACKLOG.md) calls for
porting Netflix#1406.

## Decision

Apply the upstream patch in substance:

- **Mount**: walk a local `VmafPredictModel *head` cursor without
  mutating the list; handle the empty-list special case separately.
- **Unmount**: walk with a `prev`/`head` pair; on match, splice out
  (either updating `prev->next` or `feature_collector->models`) and
  return 0. Return `-ENOENT` when the model is not mounted.

Test coverage is extended to exercise a 3-element mount / unmount
sequence and verify insertion-order preservation. The upstream test
extension duplicated ~60 LoC across two test bodies; we refactor to
a shared `load_three_test_models()` / `destroy_three_test_models()`
helper pair to keep each test under the JPL-Power-of-10 rule-4
size threshold.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Port upstream diff verbatim (with its duplicated test setup) | Smallest semantic divergence from upstream PR | Two test bodies > 60 LoC each — trips clang-tidy readability-function-size. Also carries upstream's multi-decl (`VmafModel *m0, *m1, *m2;`) style that conflicts with SEI CERT DCL04-C. | Fork lint gates would have flagged it |
| Apply this ADR's version (shared helper + per-test loop over `models[3]`) | Same correctness coverage, half the LoC per test, single-responsibility helper; no clang-tidy warnings | Fork-local reshape of the upstream test | Chosen |
| Return `-ENOENT` vs keep `-EINVAL` | `-ENOENT` matches POSIX convention for "entry not present" | Callers that previously checked `err == -EINVAL` only on NULL inputs now need to distinguish; no in-tree caller does | Align with upstream convention — any future caller gets clean error semantics |
| Defer until upstream merges #1406 | No fork-local diff to carry | PR has been open since 2025-02-02; correctness bug lives in fork in the meantime | Ship the fix now; `/sync-upstream` merges trivially later |

## Consequences

- **Positive**: Mounting 3+ models no longer corrupts the list.
  `unmount_model` returns a meaningful error code on misuse. Test
  suite now exercises multi-model scenarios (was single-model only).
- **Negative**: `vmaf_feature_collector_unmount_model` error-code
  change (`-EINVAL` → `-ENOENT` on not-found) is technically a
  behaviour change, but the only internal caller treats any non-zero
  as failure, so behaviour is preserved.
- **Neutral / follow-ups**:
  - Shared `load_three_test_models()` helper lives in
    `core/test/test_feature_collector.c`; not promoted to a
    general test utility (only one consumer).
  - On upstream merge of #1406, the fork's line count will differ
    slightly due to the shared-helper refactor; the conflict will be
    in the test file and resolves in favour of fork's cleaner form
    (document in `rebase-notes.md`).

## References

- Upstream PR: [Netflix#1406 — Bugfix: adding/removing models][pr1406]
- Backlog: [`.workingdir2/BACKLOG.md`](../../.workingdir2/BACKLOG.md) T4-4
- Audit: [`.workingdir2/analysis/upstream-backlog-audit.md`](../../.workingdir2/analysis/upstream-backlog-audit.md)
  PR #1406
- Source: `req` — user direction to ship Batch-A (T0-1 + T4-4/5/6) as
  one PR (2026-04-20 popup).

[pr1406]: https://github.com/Netflix/vmaf/pull/1406
