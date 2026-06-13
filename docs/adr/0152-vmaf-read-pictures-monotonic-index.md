# ADR-0152: `vmaf_read_pictures` rejects non-monotonic indices

- **Status**: Accepted
- **Date**: 2026-04-24
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: api, correctness, motion, netflix-upstream

## Context

`vmaf_read_pictures(vmaf, ref, dist, index)` forwards the picture
pair to each registered feature extractor. Several extractors
maintain sliding-window internal state keyed by `index % N`:

- `integer_motion` (motion, motion2, motion3) in
  [`core/src/feature/integer_motion.c`](../../core/src/feature/integer_motion.c)
  holds a 3-frame Gaussian-blur ring plus a 5-frame window; it
  indexes the ring with
  `const unsigned blur_idx_0 = (index + 0) % 3;`
  `const unsigned blur_idx_1 = (index + 1) % 3;`
  `const unsigned blur_idx_2 = (index + 2) % 3;`.
- `integer_motion_v2` in
  [`integer_motion_v2.c`](../../core/src/feature/integer_motion_v2.c)
  keeps a single previous frame (`prev_ref` in the `VmafContext`
  plus the extractor's own `prev`).

Netflix upstream issue
[#910](https://github.com/Netflix/vmaf/issues/910) reports the
downstream symptom: submitting frames `3970, 3974, 3972, 3973`
(in that order) and then flushing produces a final JSON where
frame 3974 is missing its `integer_motion2_score`. The root cause
is that the ring-buffer slot the motion extractor expects to hold
"frame index-1 blur" now holds whatever frame was submitted at
the modulo-matching position in submission order rather than
frame order, so the SAD reduction between `blur[index-1]` and
`blur[index]` reads garbage. The Netflix issue's resolution
suggestion from 2021-10-14:

> Documentation should probably be added to the API to warn of
> this, and `picture_read()` should probably check for
> monotonically increasing index to avoid inadvertent bad
> results from being created.

No guard has been added upstream since. Fork state pre-this-PR
matches upstream — `vmaf_read_pictures` accepts any index,
including duplicates and regressions.

## Decision

Enforce a monotonically-increasing index contract at the API
boundary in `vmaf_read_pictures`:

1. Add two fields to `VmafContext` in
   [`core/src/libvmaf.c`](../../core/src/libvmaf.c):
   `unsigned last_index` + `bool have_last_index`. Zero-
   initialised by `vmaf_init` (every other `VmafContext` field
   is already `memset(…, 0, …)`'d on init).
2. In the existing `read_pictures_validate_and_prep` helper
   (introduced in T7-5 / ADR-0146), prepend a check:

   ```c
   if (vmaf->have_last_index && index <= vmaf->last_index)
       return -EINVAL;
   ```

3. After the helper's existing validation passes, set
   `vmaf->last_index = index; vmaf->have_last_index = true;`
   so subsequent calls see the guard.
4. Add a unit test
   [`core/test/test_read_pictures_monotonic.c`](../../core/test/test_read_pictures_monotonic.c)
   with three subtests:
   - strictly-increasing indices with gaps are accepted;
   - duplicate indices are rejected with `-EINVAL`;
   - the Netflix#910 reproducer sequence (3970, 3974, 3972, 3973)
     rejects the two out-of-order submissions and accepts the
     next increasing index (3975).
5. Register the test in
   [`core/test/meson.build`](../../core/test/meson.build).

The contract's semantic shape:

- **Strictly increasing**: `index > last_index` is required.
- **Gaps are allowed**: the only requirement is that `index` is
  greater than `last_index`, not that `index == last_index + 1`.
  This matches how existing callers use the API (e.g. encoder
  pipelines drop frames on errors and keep counting).
- **Duplicates are rejected**: same index twice returns
  `-EINVAL` because the sliding-window state would be
  ambiguous.
- **Regressions are rejected**: any index below the previously-
  accepted one returns `-EINVAL`.
- **Flush is separate**: `vmaf_read_pictures(vmaf, NULL, NULL,
  0)` routes to `flush_context` before the guard runs, so
  flushing remains always-available.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Reorder frames internally** | Users could submit any order and still get correct scores | Significant memory + complexity; changes pipeline semantics (no streaming — must buffer the entire window before extracting); doesn't match encoder-pipeline natural frame order | Rejected — the bug is the contract, not the scheduler |
| **Log warn but process** | Non-breaking API | Still produces silently-wrong scores for motion / motion2 / motion3; loud warnings drowned out by other logging | Rejected — "produce correct output or fail loudly" is the fork's preference |
| **Document only, no runtime guard** | Smallest diff; exactly what upstream's 2021 comment said | Existing callers aren't reading new docs; bugs ship regardless. The whole point of enforcing contracts is that docs aren't a mechanism | Rejected — documentation without enforcement is not a contract |
| **Ignore Netflix#910 upstream and add index-monotonicity only for fork-local tests** | Zero public-API impact | Silent miscomputation in production is a correctness regression, not a "fork-local concern" | Rejected — this is an API-boundary bug that deserves an API-boundary fix |

## Consequences

- **Positive**:
  - The Netflix#910 symptom — missing
    `integer_motion2_score` on the last frame when frames are
    submitted out of order — is turned from a silent-wrong-
    answer into an `-EINVAL` at the API call site. Callers that
    check return values catch the bug immediately.
  - The `index % N` sliding-window state across every future
    extractor that keys on index is now safe by construction.
    Future extractors can assume `index` is strictly
    increasing.
  - Zero impact on properly-behaved callers — every use site in
    the fork's own CLI (`tools/vmaf.c`) and in the test suite
    already iterates with a strictly-increasing `i`.
- **Negative**:
  - **API contract change**: downstream integrations that
    previously submitted duplicate or out-of-order indices (by
    accident or on purpose) will now see `-EINVAL` where they
    previously got silent-wrong-answer. This is a visible
    behaviour change. The previous behaviour was ill-defined
    (silent corruption of motion/motion2/motion3); the new
    behaviour is well-defined (explicit rejection). Documented
    in CHANGELOG under `### Changed` to surface it.
  - Users that *want* to re-process a failed frame by resubmitting
    the same index cannot do so directly — they must either
    reset the context (`vmaf_close` + `vmaf_init`) or track the
    next-index themselves and skip forward.
- **Neutral / follow-ups**:
  - Upstream Netflix#910 is still OPEN. If upstream lands a
    similar guard, the fork's version should merge cleanly on
    `/sync-upstream` (the guard is in a fork-local helper, not
    in verbatim upstream code). If upstream instead lands
    internal reordering, the fork may want to revisit this
    design.

## Verification

- `meson test -C build` → 33/33 pass (was 32/32; one new test
  file registered).
- `meson test -C build test_read_pictures_monotonic` → 3/3
  subtests pass: `accepts_increasing`, `rejects_duplicate`,
  `rejects_out_of_order`.
- **Reducer behaviour confirmed**: temporarily reverting the
  guard (via `git stash` on `core/src/libvmaf.c`) and
  re-running the test reports `Fail: 1` — the test is a real
  reducer, not a tautology.
- `clang-tidy -p build core/src/libvmaf.c` → zero warnings
  (the new state fits inside the existing
  `read_pictures_validate_and_prep` helper; function size
  unchanged).

## References

- Upstream issue:
  [Netflix/vmaf#910](https://github.com/Netflix/vmaf/issues/910)
  ("Flushing doesn't compute metrics for last frame when frames
  are submitted out of order"), OPEN as of 2026-04-24.
- Upstream PR that motivated the out-of-order use case but did
  not fix the contract: Netflix#726.
- Backlog: `.workingdir2/BACKLOG.md` T1-2.
- [ADR-0146](0146-nolint-sweep-function-size.md) — introduced
  the `read_pictures_validate_and_prep` helper that this ADR
  extends.
- User direction 2026-04-24 popup: "T1-2 out-of-order flush
  misses last frame (Netflix#910)".
