<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1008: Fix C lifecycle bugs — pic_cnt double-increment, div-by-zero in pooled score, silent test failures

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `core`, `correctness`, `test`, `c`

## Context

Three correctness defects identified by the r3/r4 audit:

1. **`libvmaf.c:2507` / `2576`** — `vmaf->pic_cnt` is incremented before
   `read_pictures_validate_and_prep` / `vmaf_sycl_queue_wait` succeeds.  If the
   caller retries on a transient error (e.g. `-ENOMEM` from picture-pool exhaustion),
   `pic_cnt` is double-incremented, corrupting the FPS calculation and the
   `index_high` value passed to `vmaf_score_pooled`.  Same pattern in the SYCL path.

2. **`libvmaf.c:2762-2774`** — `vmaf_feature_score_pooled` loops over
   `[index_low, index_high]` and increments `pic_cnt` only for frames that are not
   subsampled (`n_subsample`). When `n_subsample` is large enough that every frame
   in the range is skipped, `pic_cnt` stays 0; `sum / pic_cnt` and
   `(double)pic_cnt / i_sum` then divide by zero / produce NaN.

3. **`test_feature_collector.c:236,239`** — `score = 105.` / `score = 109.` are
   assignments (`=`), not equality comparisons (`==`).  The `mu_assert` condition is
   always non-zero (truthy), so the aggregate-score value checks never exercise the
   actual score.

4. **`test_framesync.c:77`** — The framesync verification loop compared
   `dependent_buf[ctr]` against `ref+dist` but the writer stored `ref+dist+2` (line
   56).  The mismatch triggered on every non-zero-seeded frame, but the error was
   only printed to `stderr` via `fprintf` — invisible to the test harness.  The test
   always "passed" regardless of data corruption.

## Decision

1. Move `vmaf->pic_cnt++` to after the success check in both `vmaf_read_pictures`
   and `vmaf_read_pictures_sycl`.
2. Add `if (pic_cnt == 0) return -EINVAL;` before the `switch` in
   `vmaf_feature_score_pooled`; also guard `i_sum > 0.` in the harmonic-mean case.
3. Fix `score = 105.` → `score == 105.` and `score = 109.` → `score == 109.` in
   `test_feature_collector.c`.
4. Fix the off-by-2 in `test_framesync.c` (check `ref+dist+2`, matching the writer)
   and replace the silent `fprintf` with `abort()` so data corruption terminates the
   test process.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Leave pic_cnt before validate | No diff | Silent FPS corruption on retry | Rejected |
| Return NaN from pooled score on zero pic_cnt | Preserves partial result | NaN propagates silently into scores | Rejected: -EINVAL is cleaner |
| Use `assert()` in framesync test | Standard | Does not show which byte failed | abort() with fprintf gives more info |

## Consequences

- **Positive**: FPS calculation is correct on error retries; pooled score returns an
  explicit error instead of NaN/Inf; test assertions now exercise real values; framesync
  data corruption is no longer silent.
- **Neutral**: No golden-assertion values change; no public ABI change.
- **Negative**: None.

## References

- r3/r4 audit findings: `[r3-resource-exhaustion]`, `[r4-test-quality]`
- ADR-0141 (touched-file cleanup rule)
