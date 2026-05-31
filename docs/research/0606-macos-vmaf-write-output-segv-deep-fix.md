<!-- markdownlint-disable MD036 -->
# Research digest: macOS SIGSEGV deep-fix in output.c writers (ADR-0606)

_2026-05-19 — lusoris / Claude_

## Summary

PR #1403 (ADR-0602) was supposed to fix the macOS SIGSEGV in
`test_output::test_write_output_json_path` and
`test_public_api_score::test_vmaf_write_output`. The macOS CI leg was
cancelled and the fix was never verified. CI run 26065652545 (job
76635756665, `2026-05-19T04:10:25.973Z`) confirmed the same two tests still
SIGSEGV after #1403 merged.

## Environment factors (macOS CI)

The macOS CI runs under:

- `MALLOC_PERTURB_=198` — fills newly-allocated memory with `0xC6`, freed
  memory with `0x39`. Any read from uninitialized or freed memory returns a
  byte in `[0x39, 0xC6]` rather than a safe zero.
- `UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1` — terminates the process
  on the first UB event (but only if the binary was compiled with
  `-fsanitize=undefined`, which the release build is not; this variable
  has no effect on an uninstrumented binary).
- `--buildtype release` — no sanitizer instrumentation; UB optimizations are
  **active**.

The SIGSEGV is signal 11 (memory access violation), not signal 6 (abort),
confirming it is a real invalid memory access rather than a sanitizer abort.

## Why `MALLOC_PERTURB_=198` is decisive

Linux glibc `malloc` returns pages that are zero-initialized in most cases
(or carry the previous allocator's junk). The `written` field of an
uninitialized `VmafFeatureScore` struct is typically 0, which means
bounds-check bugs don't manifest as visible errors on Linux.

On macOS, `MALLOC_PERTURB_=198` (0xC6 fill) ensures every byte of a new
allocation is non-zero. A read from `score[capacity].written` (one past the
end) returns 0xC6, which is truthy. Apple Clang's optimizer, given that the
read is technically UB (it is past the end of the allocated array), is free
to assume the access is valid and the value can be used in any way consistent
with the program's observable behavior under the C abstract machine —
including propagating it into subsequent pointer operations. This can
manifest as a SIGSEGV when a garbage pointer derived from the stale byte is
dereferenced.

## Bug catalogue

### Bug 1 — off-by-one: `i > capacity` in 7 capacity bounds checks

Every frame-iteration loop in `output.c` used:

```c
if (i > fc->feature_vector[j]->capacity)
    continue;
```

The allocated score array covers indices `0..capacity-1`. When `i ==
capacity`, `i > capacity` is false (the test is "strictly greater"), so the
code proceeds to access `score[capacity]` — one past the end of the
allocated array. This is a heap buffer overread (UB per C11 §6.5.6 ¶8).

Fix: `if (i >= fc->feature_vector[j]->capacity)` (7 sites, all in
`output.c`).

**When does this trigger?**

In `seed_normal()` (used by `test_write_output_json_path`), both "feat_a"
and "feat_b" have the same capacity (8), so `max_capacity` returns 8 and the
loop goes `i = 0..7`. At `i = 7`, the check `7 >= 8 = true` (new code,
correct skip) vs `7 > 8 = false` (old code, accesses `score[7]` which IS
within bounds for capacity 8). So in the homogeneous case (all features same
capacity), the old `>` code is actually safe.

However, `make_vmaf_ctx_with_scores()` (used by `test_vmaf_write_output`)
calls `vmaf_model_load` which registers a large number of features with
different capacities. In this heterogeneous case, feature `j` with capacity
`N_j` and `max_capacity(fc) > N_j` will be accessed at index `N_j` when
`i == N_j`, triggering the OOB read.

Additionally, the `count_written_at` function is called for every `i` and is
the gating predicate for `json_write_frame`. An OOB read that returns 0xC6
(truthy) from the `written` field can cause `json_write_frame` to be called
for frame indices that have no valid data, which then tries to iterate
features at that index and may dereference garbage pointers.

### Bug 2 — fps `0.0/0.0`

```c
const double fps =
    vmaf->pic_cnt /
    ((double)(vmaf->feature_collector->timer.end - vmaf->feature_collector->timer.begin) /
     CLOCKS_PER_SEC);
```

When `pic_cnt == 0` and `timer.begin == timer.end` (common for
import-only callers), this evaluates to `0.0 / 0.0 = NaN`.

IEEE-754 defines `0.0 / 0.0` as a quiet NaN (qNaN). The JSON/XML writers'
`fpclassify()` switch handles NaN by writing `null` or `0`. However, Apple
Clang under a strict FP environment may generate a SIGFPE on the division
itself before any NaN representation can be used. The risk is platform- and
flag-dependent, but the defensive fix (explicit zero-division guard) is
correct hardening regardless.

### Bug 3 — `json_write_pool_score` comma logic: `j > 1`

```c
static void json_write_pool_score(FILE *outfile, unsigned j, double score, const char *sf)
{
    fprintf(outfile, "%s", j > 1 ? ",\n" : "\n");
```

`j` is the `VmafPoolingMethod` enum value:

- 1 = `VMAF_POOL_METHOD_MIN`
- 2 = `VMAF_POOL_METHOD_MEAN`
- 3 = `VMAF_POOL_METHOD_HARMONIC_MEAN`

The intent: emit a leading comma for everything after the first score. The
bug: when `vmaf_feature_score_pooled` returns an error for `j == 1` and the
call is skipped, `j == 2` is the first actual score emitted but `j > 1` is
true, so it emits a leading comma — producing `{,\n "mean": ...}` which is
invalid JSON.

Fix: `bool *first` flag passed by pointer, flipped after the first emission.

### Bug 4 — `json_write_frames` separator: `i > 0`

```c
fprintf(outfile, "%s", i > 0 ? ",\n" : "\n");
```

`i` is the frame index. The intent: no comma before the first frame entry.
The bug: when frame 0 has no written scores but frame 3 does,
`count_written_at(fc, 0) == 0` and the frame 0 entry is skipped. Frame 3
is the first actually-emitted entry but `i == 3 > 0` is true, so it emits a
leading comma — producing `[\n,\n{...}]` which is invalid JSON.

Fix: `bool first_frame = true` flag, flipped after the first emission.

## Verification

Linux (with MALLOC_PERTURB_=198 simulation):

```text
MALLOC_PERTURB_=198 meson test -C core/build test_output test_public_api_score
1/2 fast - libvmaf:test_output           OK   0.00s
2/2 fast - libvmaf:test_public_api_score OK   0.01s
```

Full fast suite: 50/50 pass.

macOS CI verification expected via PR CI run.
