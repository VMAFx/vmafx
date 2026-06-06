<!-- markdownlint-disable MD013 -->
# ADR-1066: Regression tests for the sequential-realloc double-free in libsvm

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `test`, `security`, `ci`

## Context

PR #658 hardened the vendored libsvm 3.24 `svm_group_classes()` and
`svm_check_parameter()` against a CERT MEM04-C violation: the original code
overwrote a pointer with the `realloc()` return value directly, losing the
original pointer on OOM. The fix allocated into a temporary pointer first.

However, the fix introduced a new bug: both `realloc()` calls were protected
by a single combined null-check (`if (!tmp_label || !tmp_count)`). When
`realloc(label)` succeeded, libc freed the old `label` block internally and
returned a new pointer. If the subsequent `realloc(count)` then failed, the
combined branch called `free(label)` on the now-dangling original pointer — a
classic double-free (UB, immediately fatal under ASan).

PR #708 fixed both sites by separating the two `realloc()` calls so each is
checked and assigned independently before the next one is attempted.

The realloc path in `svm_group_classes()` is only reached when the number of
distinct class labels exceeds `max_nr_class` (initial value: 16). The existing
`test_svm_api.c` uses a 2-class fixture, which never crosses this threshold —
so the fixed code path had zero test coverage. The realloc path in
`svm_check_parameter()` is inside the NU_SVC feasibility check and similarly
requires 17+ classes.

This ADR records the decision to add `test_svm_multiclass.c` (17-class and
32-class C_SVC fixtures, plus a 17-class NU_SVC fixture) as a permanent
regression guard, and to add `scripts/ci/check-compose-dri-writable.sh` as a
lint gate preventing re-introduction of `read_only: true` on `/dev/dri`
bind-mounts in `dev/docker-compose.yml` (the bug fixed by PR #707).

## Decision

Add `core/test/test_svm_multiclass.c` registered as `test_svm_multiclass` in
`meson.build` (fast suite), and `scripts/ci/check-compose-dri-writable.sh`
wired into `dev-container-build.yml` as a pre-build lint step.

## Alternatives considered

<!-- markdownlint-disable MD055 MD056 -->
| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Extend `test_svm_api.c` with 17-class fixture | Fewer files | File becomes oversized; regression harder to bisect by name | Rejected — separate focused file is easier to audit |
| `meson.test()` `should_fail` + OOM injection | Directly tests failure path | Needs custom allocator or LD_PRELOAD — portability risk | Rejected — ASan in sanitizers CI job is sufficient |
| YAML parser in compose lint | More robust parse | Adds Python/Go dep to a shell-only script | Rejected — state-machine is sufficient for one-field invariant |
<!-- markdownlint-enable MD055 MD056 -->

## Consequences

- **Positive**: The fixed realloc paths in `svm.cpp` are now exercised on
  every `meson test --suite=fast` run and under ASan in the sanitizers CI job.
  Any regression to the double-free pattern aborts immediately in sanitized
  builds. The compose lint prevents re-introduction of `read_only: true` on
  `/dev/dri`.
- **Negative**: Two new test executables add a small build-time increment
  (trivial for a single-TU C file).
- **Neutral / follow-ups**: The lint script is dependency-free bash; it runs
  in under 1 second on the PR-time `dev-container-build.yml` job, before the
  expensive Docker build layer.

## References

- PR #707 (fix dev/dri read_only)
- PR #708 (fix SVM double-free)
- PR #658 (original CERT MEM04-C realloc hardening)
- ADR-0889 (libsvm vendoring + NOLINT cordon)
- ADR-0529 (NVIDIA Container Toolkit /dev/dri passthrough policy)
