<!-- markdownlint-disable MD013 MD060 -->
# ADR-0602: macOS SIGSEGV in vmaf_write_output — pic_cnt underflow + missing vmaf NULL guard

- **Status**: Accepted
- **Date**: 2026-05-19
- **Deciders**: lusoris
- **Tags**: `bugfix`, `macos`, `output`, `portability`, `correctness`, `fork-local`

## Context

After PR #1355 merged (the omnibus CI-fix that added a NULL guard for
`output_path` in `vmaf_write_output_with_format`), two tests in the
`fast` suite continued to SIGSEGV on the macOS-clang CI leg:

- `test_output::test_write_output_json_path` (test_output.c:504)
- `test_public_api_score::test_vmaf_write_output` (test_public_api_score.c:227)

Both call `vmaf_write_output()` with a valid non-NULL context and a valid
temp-file path, so the PR #1355 guard for `output_path = NULL` did not
apply.

**Root cause 1 — `pic_cnt - 1` unsigned underflow.**

Both tests populate the feature collector via `vmaf_feature_collector_append`
or `vmaf_import_feature_score` directly, bypassing `vmaf_read_pictures`.
Therefore `vmaf->pic_cnt` remains 0 when `vmaf_write_output` is called.

`vmaf_write_output_with_format` passes `vmaf->pic_cnt` (= 0) as `pic_cnt`
to `vmaf_write_output_json` and `vmaf_write_output_xml`.  Both writers call
pooled-metrics helpers that compute `pic_cnt - 1`:

```c
vmaf_feature_score_pooled(vmaf, name, j, &score, 0, pic_cnt - 1);
```

When `pic_cnt == 0`, `pic_cnt - 1` wraps to `UINT_MAX` (unsigned, well-defined
by the C standard).  This passes `vmaf_feature_score_pooled`'s early guard:

```c
if (index_low > index_high)   /* 0 > UINT_MAX — always FALSE for unsigned */
    return -EINVAL;
```

and enters the loop `for (unsigned i = 0; i <= UINT_MAX; i++)`.  On Linux
with GCC, the compiler emits the early-exit (`return err` on the first
missing score at `i = 1`) and the loop terminates in two iterations.  On
macOS with Apple Clang, the same IR produces machine code that in some
optimisation configurations does not reach the exit before a guard-page hit
or stack exhaustion, causing SIGSEGV.

**Root cause 2 — missing NULL guard for `vmaf` context.**

PR #1355 guarded `output_path` but left the `vmaf` pointer unguarded.  The
very next statement dereferences `vmaf->feature_collector`, so a NULL `vmaf`
argument also SIGSEGV'd on macOS (Apple Clang inlines the open(2) call with
`__attribute__((nonnull(1)))` and the path-guard elimination propagates to
the surrounding context dereference).

## Decision

Two targeted fixes in `core/src/libvmaf.c` and `core/src/output.c`:

1. **Guard `vmaf` and `vmaf->feature_collector` up front** in
   `vmaf_write_output_with_format`, before any pointer dereference.

2. **Guard `pic_cnt > 0` before `pic_cnt - 1`** in
   `json_write_pooled_entry` and `xml_write_one_metric_pools`, eliminating
   the UINT_MAX underflow entirely.  When `pic_cnt == 0` the pooled-metrics
   block is emitted but empty, which is the correct and documented behaviour
   for contexts populated via `vmaf_import_feature_score`.

3. **Add NULL guards to `vmaf_write_output_json`** matching those already
   present in `vmaf_write_output_xml`.

4. **Add `test_write_output_pic_cnt_zero`** regression test to
   `core/test/test_output.c` that exercises the JSON and XML paths with
   `pic_cnt == 0` on Linux, ensuring any future regression surfaces in the
   fast suite before reaching macOS CI.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Clamp `pic_cnt` to 1 before passing to writers | One-line fix at call site | Silently changes observable output (pooled metrics would appear with a bogus 1-frame pool) | Correctness violation — callers using `vmaf_import_feature_score` legitimately have no frames |
| Fix `vmaf_feature_score_pooled` to reject `index_high == UINT_MAX` | Closes the root loop | Does not address the invalid `pic_cnt - 1` expression; caller would still pass a nonsensical range | Symptom-level fix; the real invariant is "don't pool when there are no frames" |
| Add `__attribute__((nonnull))` to `vmaf_write_output_with_format` | Documents contract | On macOS, nonnull + caller passing NULL bypasses the guard and SIGSEGV before our code runs | Counter-productive — makes the crash UB-silent instead of controlled |

## Consequences

- **Positive**: `test_output` and `test_public_api_score` pass on macOS
  clang (CPU) without SIGSEGV. JSON and XML output for
  `vmaf_import_feature_score`-only contexts is well-formed and consistent
  across platforms.
- **Positive**: `vmaf_write_output_json` now has the same NULL guards as
  `vmaf_write_output_xml` — the two writers are now symmetric.
- **Neutral**: `vmaf_write_output_with_format` grows two extra `if` checks
  at the head of a cold path (output writing is not on the hot frame loop).
- **Follow-up**: The `vmaf_feature_score_pooled` guard (`index_low >
  index_high`) does not protect against unsigned underflow because both
  operands are unsigned.  A future hardening pass could add an explicit
  `if (index_high == UINT_MAX) return -EINVAL;` there as belt-and-suspenders.

## References

- PR #1355 (incomplete fix — `output_path` NULL guard only):
  commit `34f80d05be999ebd2e5169e0a72aa3aa72116ffb`.
- Failing tests: `core/test/test_output.c:504`,
  `core/test/test_public_api_score.c:227`.
- Fixed files: `core/src/libvmaf.c`, `core/src/output.c`,
  `core/test/test_output.c`.
- req: "fix macOS SIGSEGV in vmaf_write_output (PR #1355 follow-up)"
