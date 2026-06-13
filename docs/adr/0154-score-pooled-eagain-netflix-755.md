# ADR-0154: `vmaf_score_pooled` returns `-EAGAIN` for pending features

- **Status**: Accepted
- **Date**: 2026-04-24
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: api, correctness, motion, netflix-upstream

## Context

Netflix upstream issue
[#755](https://github.com/Netflix/vmaf/issues/755) reports that
`vmaf_score_pooled` fails with `-EINVAL` when called immediately
after `vmaf_read_pictures(ctx, ref, dist, i)` for the same
index `i`. The reporter's pattern was:

```c
while (!eof) {
    vmaf_read_pictures(ctx, &ref, &dist, frameIdx);
    vmaf_score_pooled(ctx, model, MEAN, &score, frameIdx, frameIdx);
    ++frameIdx;
}
vmaf_score_pooled(ctx, model, MEAN, &score, 0, frameIdx - 1);
```

Expected: running per-frame VMAF output (streaming pooled score).
Actual: the first call returns a valid score; every subsequent
per-frame call returns `-EINVAL`. Indistinguishable from
"programmer error" (bad pointer, out-of-range index), so callers
cannot tell whether to retry later or abort.

Root cause: several feature extractors write frame N's score
**retroactively**:

- `integer_motion`'s motion2/motion3 use a 3-frame sliding
  window (or 5-frame with `motion_five_frame_window`). Frame
  N's motion2 is written when frame N+1 is extracted; the
  tail is written on flush.
- Any future extractor using a look-ahead window inherits the
  same pattern.

So at the instant `vmaf_read_pictures(i)` returns, frame `i`'s
motion2 is *not yet* in the feature collector — it'll be filled
in when frame `i+1` arrives (or on flush, for the tail). The
collector reports "not written" by returning `-EINVAL` in both
`vmaf_feature_collector_get_score` (the mutex-protected path
used by `vmaf_score_pooled`) and the inline
`vmaf_feature_vector_get_score` (the cached fast-path used by
`vmaf_predict_score_at_index`, which itself returns `-1`).

Both paths collapse the distinction between three very different
failure modes:

1. **Programmer error**: bad pointer, out-of-range index,
   feature-name typo.
2. **Feature never registered**: `vmaf_use_features_from_model`
   wasn't called, or the feature_name is wrong.
3. **Feature registered, write pending**: extractor is alive but
   the requested index hasn't been filled yet.

Case 3 is the transient one — waiting for one more
`vmaf_read_pictures` call (or a flush) will resolve it. Cases 1
and 2 are fatal.

## Decision

Introduce `-EAGAIN` as the return code for case 3 only. Cases 1
and 2 continue to return `-EINVAL`.

Patch sites:

- [`core/src/feature/feature_collector.c`](../../core/src/feature/feature_collector.c)
  `vmaf_feature_collector_get_score`: the "not written" branch
  now returns `-EAGAIN`. The "out-of-range / not-found" branch
  keeps `-EINVAL`.
- [`core/src/feature/feature_collector.h`](../../core/src/feature/feature_collector.h)
  `vmaf_feature_vector_get_score` (inline fast-path): split the
  previous combined `return -1` into `-EINVAL` (null pointer /
  out-of-range) and `-EAGAIN` (not written). Add `#include
  <errno.h>` so the inline can reference the constants from
  callers of this header. Rename reserved
  `__VMAF_FEATURE_COLLECTOR_H__` guard to
  `VMAF_FEATURE_COLLECTOR_INCLUDED` (ADR-0141 drive-by).
- Public error-code behaviour: callers that treat every non-zero
  return as a fatal error are unchanged. Callers that want to
  distinguish can now branch on `-EAGAIN` and re-issue the call
  after one more `vmaf_read_pictures` or after flush.

Supported streaming pattern (documented via the new test):

```c
for (unsigned i = 0; i < N; i++) {
    vmaf_read_pictures(ctx, &ref, &dist, i);
    if (i >= 2) {
        /* i-2 is the deepest still-pending frame; 3-frame motion
         * window fills in i-2 when i arrives. */
        vmaf_score_pooled(ctx, model, MEAN, &score, i - 2, i - 2);
    }
}
vmaf_read_pictures(ctx, NULL, NULL, 0); /* flush → retroactive tail */
for (unsigned i = 0; i < N; i++)
    vmaf_score_pooled(ctx, model, MEAN, &score, i, i);
```

The lag constant `2` is specific to vmaf_v0.6.1 + its motion2
(3-frame) dependency; `motion_five_frame_window` would require a
lag of 4. Callers that want to be metric-agnostic should just
flush before pooling.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Block in `vmaf_score_pooled` until features arrive** | User gets a score; no `-EAGAIN` to handle | Frame N's motion2 requires frame N+1 *from the user*, not from a worker thread; blocking would deadlock unless paired with a timeout or a completion callback | Rejected — the bug is ordering, not concurrency |
| **Add a new `vmaf_feature_available_up_to(ctx, unsigned *i_max)` API** | Explicit; caller polls before pooling | Bigger API surface; callers still have to branch | Rejected — distinguishing `-EINVAL` vs `-EAGAIN` is the minimal thing; the polling API can still be added later if needed |
| **Silently return score = 0 for not-yet-written indices** | No error code to handle | Hides real bugs; a missing feature that stays missing never surfaces | Rejected — silent wrong-answer is the worst failure mode |
| **Keep `-EINVAL`, document the streaming pattern only** | Zero code change | Leaves existing callers unable to tell transient from fatal; upstream maintainer's stance in the issue thread — "you cannot call `vmaf_score_pooled()` in a loop" — is effectively a won't-fix | Rejected — the signal is cheap and strictly improves downstream code |

## Consequences

- **Positive**:
  - Downstream integrations that want per-frame VMAF streaming
    can now distinguish "not yet" (retry after next read or
    after flush) from "will never succeed" (abort). Upstream's
    2020 maintainer response closed the door on this; the fork
    opens it with one changed error code and one inline-helper
    signature tweak.
  - The inline fast-path and the mutex-protected path now share
    the same error discipline (`-EINVAL` for structural
    problems, `-EAGAIN` for transient pending writes).
  - Drive-by: reserved-identifier header guard
    `__VMAF_FEATURE_COLLECTOR_H__` renamed to
    `VMAF_FEATURE_COLLECTOR_INCLUDED` (ADR-0141 touched-file
    rule).
- **Negative**:
  - **Visible behaviour change** at the public API: callers who
    currently branch on exact `-EINVAL` for the transient case
    (unlikely — nobody should be matching a specific fatal-error
    code against a transient condition, but possible) will see
    a different code. Flagged under `### Changed` in CHANGELOG.
  - `vmaf_feature_vector_get_score` previously returned a literal
    `-1`; now returns `-EINVAL` (`-22` on Linux). Same class of
    change; same flag in CHANGELOG.
- **Neutral / follow-ups**:
  - Upstream Netflix#755 is still OPEN. If upstream lands a
    similar distinction later (unlikely given the 2020 response
    closing the door), the fork's return-code semantic will
    already match; merge cleanly on `/sync-upstream`.
  - A future `vmaf_feature_available_up_to` polling API could
    build on this — the `-EAGAIN` signal is the minimal contract
    that lets downstream write the exact stream-vs-block tradeoff
    that suits them.

## Verification

- `meson test -C build` → **35/35 pass** (was 34; one new test
  file added).
- `meson test -C build test_score_pooled_eagain` → 4/4 subtests
  pass:
  - `test_score_pooled_returns_eagain_on_pending` (pins
    `-EAGAIN`, not `-EINVAL`, for a pending motion2 index).
  - `test_score_pooled_streaming_pattern` (pins that
    `score_pooled(i-2, i-2)` after `read_pictures(i)` returns
    0 with a finite score).
  - `test_score_pooled_after_flush_complete` (pins that every
    in-range index is poolable after flush).
  - `test_score_pooled_still_rejects_bad_range` (pins that
    programmer-error cases — inverted range, NULL score — still
    return `-EINVAL`, not `-EAGAIN`).
- **Reducer verified**: `git stash push
  core/src/feature/feature_collector.{c,h} && ninja -C
  build && meson test -C build test_score_pooled_eagain`
  reports `Fail: 1` — the test is a real gate, not a tautology.
- Reproducer standalone binary (matches the issue's code
  pattern):

  ```text
  read_pictures(0) -> 0
  score_pooled(0,0) -> rc=0 score=97.428043
  read_pictures(1) -> 0
  score_pooled(1,1) -> rc=-11 score=97.428043   # -EAGAIN
  read_pictures(2) -> 0
  score_pooled(2,2) -> rc=-11 score=97.428043   # -EAGAIN
  flush -> 0
  final_pool(0..2) -> rc=0 score=97.428043
  ```

- `clang-tidy -p build core/src/feature/feature_collector.c
  core/src/feature/feature_collector.h` → zero warnings.

## References

- Upstream issue:
  [Netflix/vmaf#755](https://github.com/Netflix/vmaf/issues/755)
  ("VMAF functions exit with error when vmaf_read_pictures and
  vmaf_score_pooled calls are interleaved"), OPEN as of
  2026-04-24.
- Related upstream issue:
  [Netflix/vmaf#712](https://github.com/Netflix/vmaf/issues/712)
  (earlier report of the same pattern).
- Backlog: `.workingdir2/BACKLOG.md` T1-1.
- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file
  lint rule (drove the header-guard rename).
- [ADR-0148](0148-iqa-rename-and-cleanup.md) — precedent for
  `_INCLUDED` header-guard renames.
- User direction 2026-04-24 popup: "T1-1
  vmaf_read_pictures/score_pooled interleave bug".
