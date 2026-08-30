<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1039: Fix CERT MEM04-C realloc OOM safety in vendored libsvm

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `memory-safety`, `correctness`, `vendored`

## Context

The vendored libsvm implementation (`core/src/svm.cpp`) contained three instances of
the CERT MEM04-C defect: assigning the return value of `realloc` directly to the pointer
being reallocated:

```c
// UNSAFE — if realloc returns NULL (OOM), the original allocation is lost and
// subsequent code writes through the NULL pointer (undefined behaviour).
label = (int *)realloc(label, max_nr_class * sizeof(int));
count = (int *)realloc(count, max_nr_class * sizeof(int));
```

The three affected call sites are:

1. `Cache::get_data` — kernel cache resize path; triggered on any cache miss once
   the initial `size_` budget is exhausted.
2. `svm_group_classes` — class-label array resize when a training problem has more
   than 16 unique classes (doubles the array each time).
3. `svm_check_parameter` — same pattern, same trigger.

On OOM (very low probability under normal use but possible on constrained hosts or
with very large training problems), `realloc` returns `NULL`, the original allocation
is lost (leak), and the immediately following array-index write (`label[nr_class] = ...`)
dereferences NULL — undefined behaviour, typically a crash.

## Decision

Fix all three call sites using the safe realloc idiom:

1. Save the return value in a temporary pointer.
2. Check for NULL; if NULL, free the original pointer (to avoid the leak), log to
   `stderr`, and call `abort()`. The vendored code has no error-propagation model
   (no return codes from `get_data`; callers of `svm_group_classes` and
   `svm_check_parameter` don't check for failure); abort is the only honest response.
3. Assign the temporary to the original pointer only after the NULL check passes.

The `abort()` strategy is consistent with the existing `Malloc` macro behaviour
throughout this file, which already silently produces a NULL-deref on OOM.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Return error code upstream | Correct recovery | Requires refactoring entire call chain (20+ functions) | Out-of-scope for a vendored file; too disruptive |
| NOLINT with comment | No code change | Does not fix the defect; CERT violation persists | Unacceptable per project standards |
| `abort()` on NULL check | Consistent with existing Malloc behaviour; safe fail-fast | Process terminates on OOM | Same as existing behaviour; makes the failure visible |

## Consequences

- **Positive**: Eliminates three CERT MEM04-C defects; prevents silent memory leaks
  and NULL-deref UB on OOM; clang-tidy and cppcheck findings for these lines clear.
- **Negative**: The `abort()` path is now reachable if the host runs out of memory
  during a very large SVM training operation. This was always the case (via NULL deref)
  but is now explicit.
- **Neutral / follow-ups**: The `Malloc` macro itself does not check for NULL; a
  complete hardening of this vendored file would address that too, but is deferred.

## References

- CERT MEM04-C: Do not perform zero-length allocations
- r7 review findings: [r7-vendored-libsvm] svm.cpp realloc OOM × 3
