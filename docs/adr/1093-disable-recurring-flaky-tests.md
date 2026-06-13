<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1093: Disable two recurring-failure tests via should_fail while root cause is under investigation

- **Status**: Accepted
- **Date**: 2026-06-07
- **Deciders**: Lusoris
- **Tags**: ci, testing, flaky, picture-pool, sycl, fork-local

## Context

Two C unit tests have each had three or more incomplete fix attempts and continue
to fail in CI on the current master tip (6358f2e10):

- **`test_pic_preallocation::test_picture_pool_basic`** — fails with "problem
  during vmaf_read_pictures". PRs #765, #769, and #797 each resolved a distinct
  layer (PREV_REF refcount leak, dict-leak via `is_initialized` guard, stale
  sanitizer deselect), but the test still fails in at least one CI configuration
  after all three were merged.
- **`test_sycl_motion_add_uv_parity`** — fails with SIGSEGV. PRs #768 and #796
  attempted to fix the SYCL motion add-UV path (ADR-0989), but the SIGSEGV
  persists in CI.

Both tests are in the `fast` suite and therefore block every PR's CI gate.
Leaving them in the blocking default state is the wrong tradeoff: they are
preventing unrelated PRs from merging while the root cause remains unknown.
At the same time, deleting the test source files would erase the regression
surface and make a future fix harder to verify.

## Decision

We will add `should_fail: true` to the two `test()` registrations in
`core/test/meson.build`. Meson's `should_fail` flag runs the test binary
and expects it to exit non-zero (or crash); a crash or non-zero exit becomes
a PASS, while an unexpected success becomes a FAIL. This is the least invasive
option:

- The test binaries are still compiled on every build — build regressions
  remain visible.
- The test still runs on every `meson test` / CI invocation — new failure
  modes remain visible through Meson's UNEXPECTEDPASS signal.
- CI stays green for unrelated PRs.
- The test source files are untouched — the fix author can iterate locally.

Each `test()` call carries an inline comment citing this ADR and the pattern
of recurring failures, so future contributors understand the flag is intentional
and temporary. Removal instruction: delete the `should_fail: true` line and the
comment block once the underlying defect is confirmed fixed and all CI legs pass.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **`should_fail: true` on the test() call** *(chosen)* | Least invasive; binary still built + run; UNEXPECTEDPASS fires if fixed | Meson `should_fail` is somewhat obscure; the fix author must also remove the flag | Preserves full regression surface at minimal friction. |
| **Remove the `test()` call but keep executable()** | Binary still compiles; test simply not scheduled | Binary is silently not exercised in CI at all; harder to notice when it transitions from broken to fixed | Loses CI run visibility. |
| **Add to sanitizer EXCLUDE lists only** | CI stays green in sanitizer lanes | These tests fail in the non-sanitizer fast-suite lane too; EXCLUDE lists only cover the sanitizer job | Does not address the problem in the primary CI gate. |
| **Delete test source files** | Completely removes blocking CI failure | Destroys the regression surface; fix author must recreate the test | User instruction explicitly prohibits this ("do not delete the test source files"). |
| **Fourth/fifth fix attempt before disabling** | Would fix root cause | Three attempts per test have already passed without resolution; another attempt without new diagnostic data risks another incomplete fix | Insufficient diagnostic information is available; the correct next step is containment now, root-cause investigation in dedicated follow-up. |

## Consequences

- **Positive**: unrelated PRs are no longer blocked by two tests with
  unresolved root causes.
- **Positive**: both test binaries continue to compile and run, preserving
  the full regression surface and the ability to detect the UNEXPECTEDPASS
  transition when the defect is fixed.
- **Negative**: `test_pic_preallocation` and `test_sycl_motion_add_uv_parity`
  no longer provide a green signal for the conditions they were designed to
  guard. This gap is tracked in `docs/state.md`.
- **Neutral / follow-ups**: the `should_fail` flags must be removed once the
  underlying defects are fixed and confirmed green across all CI legs. Each
  flag carries an inline comment with that instruction.

## References

- ADR-0989 (SYCL motion add-UV): [0989-sycl-motion-add-uv.md](0989-sycl-motion-add-uv.md)
- ADR-0347 (sanitizer matrix test scope): [0347-sanitizer-matrix-test-scope.md](0347-sanitizer-matrix-test-scope.md)
- Related PRs: #765, #769, #797 (pic-preallocation), #768, #796 (sycl-motion-add-uv)
- Source: `req` ("3rd attempt at fix all incomplete — disable 2 recurring failing
  tests with documented TODO + ADR")
