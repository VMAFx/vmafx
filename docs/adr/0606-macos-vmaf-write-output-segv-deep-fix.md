<!-- markdownlint-disable MD013 MD060 -->
# ADR-0606: macOS SIGSEGV deep-fix in output.c writers (PR #1403 follow-up)

- **Status**: Accepted
- **Date**: 2026-05-19
- **Deciders**: lusoris
- **Tags**: `bugfix`, `macos`, `output`, `portability`, `correctness`, `fork-local`

## Context

PR #1403 (commit `798a202fe`, merged 2026-05-19) added `pic_cnt > 0` guards in
`json_write_pooled_entry` and `xml_write_one_metric_pools` and NULL guards in
`vmaf_write_output_with_format` as ADR-0602's decided fix. The macOS CI leg was
cancelled for that PR and never ran. CI run 26065652545 (job 76635756665,
`2026-05-19T04:10:25.973Z`) confirmed the same two tests still SIGSEGV on
macOS clang after #1403:

- `libvmaf:test_output::test_write_output_json_path`
- `libvmaf:test_public_api_score::test_vmaf_write_output`

Both tests call `vmaf_write_output()` with a fully valid context that was
populated via `vmaf_feature_collector_append` / `vmaf_import_feature_score`
directly (bypassing `vmaf_read_pictures`), so `vmaf->pic_cnt == 0`.

**Root cause — off-by-one in capacity bounds check (7 sites).**

Every frame-iteration loop in `output.c` uses `i > fc->feature_vector[j]->capacity`
to skip features that have not been allocated up to frame `i`. The correct predicate
is `i >= capacity` because the allocated score array covers indices `0..capacity-1`;
index `capacity` is one past the end of the allocation.

With `i > capacity` (old code), when `i == capacity`, the check evaluates to false
and the code proceeds to read `score[capacity]` — a heap buffer overread (UB). Under
`MALLOC_PERTURB_=198` (set by macOS CI, value 0xC6 fill for new allocations,
0x39 for freed memory), the stale or poison byte at `score[capacity].written` can
evaluate as non-zero, causing the loop to report spurious "written" entries. Apple
Clang's optimizer, given the UB, is free to assume the access is valid and propagate
its result into subsequent control flow, which can produce a SIGSEGV via a stale
or garbage pointer dereference downstream.

The affected sites are: `count_written_at`, `xml_write_frames`, `json_write_frame`,
`vmaf_write_output_csv` (×2), and `vmaf_write_output_sub` (×2).

**Secondary issue — fps 0.0/0.0 when pic_cnt == 0.**

`vmaf_write_output_with_format` computes:

```c
const double fps = vmaf->pic_cnt /
    ((double)(timer.end - timer.begin) / CLOCKS_PER_SEC);
```

When `pic_cnt == 0` and `timer.begin == timer.end` (no frames processed),
this is `0.0 / 0.0 = NaN`. The JSON writer's `fpclassify()` switch handles
NaN correctly on Linux (IEEE-754 quiet NaN), but Apple Clang under a strict
FP environment or with `-ffast-math` may generate a SIGFPE on the 0.0/0.0
division instead of a quiet NaN.

**Tertiary issue — comma-placement bugs in JSON writers.**

`json_write_pool_score` used `j > 1` (the pool-method enum value) to decide
whether to emit a leading comma, which is wrong when the `j == 1`
(VMAF_POOL_METHOD_MIN) call returns an error and is skipped: the `j == 2`
call would emit a leading comma with no preceding value, producing malformed
JSON. Similarly, `json_write_frames` used `i > 0` (the frame index) as a
separator condition, which is wrong when the first written frame has a
non-zero index (e.g. when frame 0 has no scores but frame 3 does).

## Decision

Apply four targeted fixes across `core/src/output.c` and `core/src/libvmaf.c`:

1. **`>` → `>=` in all seven capacity bounds checks** in `output.c`, fixing the
   heap-buffer-overread UB. Add `/* ADR-0606: >= not > */` comments at each site.

2. **Defensive fps computation** in `vmaf_write_output_with_format`: compute
   `timer_elapsed` separately, guard `if (vmaf->pic_cnt == 0 || timer_elapsed == 0)`
   and return `fps = 0.0` explicitly in those cases.

3. **`json_write_pool_score` comma tracking**: change signature to accept
   `bool *first` and flip it after the first successful emission, replacing the
   `j > 1` heuristic with an accurate "has anything been written yet" flag.

4. **`json_write_frames` separator tracking**: introduce `bool first_frame = true`
   before the frame loop; emit `"\n"` for the first written frame and `",\n"` for
   subsequent ones, replacing the `i > 0` heuristic.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Clamp capacity check to `>= capacity - 1` | One char change | Identical semantics if capacity == 0 (underflow), doesn't fix the root issue | Incorrect; >= is the right fix and should be stated clearly |
| Add `__attribute__((no_sanitize("address")))` to suppress ASan | Silences CI noise | Hides a real UB; does not prevent the crash on uninstrumented macOS CI | Wrong approach; fixing the UB is always preferable to suppressing detection |
| Wrap the fps expression in `isnan()` post-hoc | Minimal diff | Does not prevent SIGFPE on platforms where 0.0/0.0 traps before the NaN is produced | Must guard before the division, not after |
| Replace `j > 1` / `i > 0` comma logic with `fprintf` separator strings | Simpler diff | Does not address the fundamental "first item" tracking problem; same class of bug resurfaces on edge cases | Explicit `bool first` flag is clearest and least error-prone |

## Consequences

- **Positive**: `test_output::test_write_output_json_path` and
  `test_public_api_score::test_vmaf_write_output` pass on macOS clang without
  SIGSEGV.
- **Positive**: JSON output is now well-formed even when pool method enum values
  have gaps or frame index 0 has no scores.
- **Positive**: fps computation emits a well-defined 0.0 rather than NaN / SIGFPE
  for import-only contexts.
- **Neutral**: Seven comments added to capacity checks increase diff size; no
  runtime cost.
- **Follow-up**: `vmaf_feature_score_pooled`'s tautological-loop issue
  (`for (unsigned i = 0; i <= UINT_MAX; i++)`) remains latent. It is now
  unreachable for `pic_cnt == 0` callers (the `pic_cnt > 0` guard from ADR-0602
  prevents it), but the belt-and-suspenders fix of adding
  `if (index_high == UINT_MAX) return -EINVAL;` there is still advisable as a
  follow-up hardening pass.

## References

- PR #1403 (partial fix — ADR-0602 guards, macOS CI cancelled):
  commit `798a202fe`.
- Failing CI run: 26065652545, job 76635756665, `2026-05-19T04:10:25.973Z`.
- Predecessor: [ADR-0602](0602-macos-vmaf-write-output-segv.md).
- Fixed files: `core/src/output.c`, `core/src/libvmaf.c`.
- req: "fix macOS SIGSEGV in test_output + test_public_api_score (PR #1403 follow-up, ADR-0602 deep-fix)"
