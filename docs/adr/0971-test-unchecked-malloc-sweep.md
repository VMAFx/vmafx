<!-- markdownlint-disable MD013 MD060 -->
# ADR-0971: Test suite: NULL-check malloc in 3 test files (Round 27 audit D.1)

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: Lusoris
- **Tags**: `testing`, `correctness`, `asan`, `fork-local`

## Context

A Round 27 audit identified 10 unchecked `malloc()` / `calloc()` call sites
across three C unit tests. Each site allocated a buffer and immediately
dereferenced it without a NULL check, causing SIGSEGV under ASan's
`MALLOC_PERTURB_` mode or any OOM condition:

- `core/test/test_ssimulacra2_simd.c` — 6 groups (`test_multiply`,
  `test_xyb`, `test_downsample`, `test_ssim`, `test_edge`, `test_blur`),
  plus 2 additional sites in `test_ptlr_one` found while touching the file.
- `core/test/test_framesync.c` — 2 sites (`pic_a` / `pic_b`) inside a
  per-frame loop.
- `core/test/test_pic_preallocation.c` — 2 sites (`data_ptrs`) inside
  per-thread setup loops.

The project runs tests under ASan + UBSan + TSan (ADR-0015). With
`MALLOC_PERTURB_=198`, ASan can force `malloc` to return NULL and immediately
expose these defects. Unchecked allocations in test code also violate the
project-wide coding standard (SEI CERT MEM32-C) that every `malloc` return
value must be checked.

## Decision

Add NULL checks immediately after each `malloc` / `calloc` group using the
established project idiom:

- For single-TU SIMD tests (`test_ssimulacra2_simd.c`): consolidated
  `if (!ptr1 || !ptr2 || ...) { free(each); return "malloc failed"; }`.
- For loop-local allocations (`test_framesync.c`, `test_pic_preallocation.c`):
  `mu_assert("malloc failed ...", ptr)` immediately after the call, which
  propagates the failure through the `mu_run_test` harness.

These patterns match the existing precedent in `test_integer_adm_simd.c`,
`test_vif_simd.c`, and `test_moment_simd.c`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `mu_assert(ptr != NULL)` for all sites | Uniform idiom | Leaks sibling allocations already made in the same function when check fires | Used only for single-alloc loop-local cases where no sibling alloc precedes |
| `exit(1)` / `abort()` on failure | Immediate, obvious | Crashes the whole test process; obscures which test failed; ASan may not flush output | Not the project idiom |
| Migrate to `simd_bitexact_test.h` helper | Centralises the pattern | `test_ssimulacra2_simd.c` is an intentional non-migrated file (fill_random FP rounding order is load-bearing per AGENTS.md) | Deferred |

## Consequences

- **Positive**: tests survive ASan `MALLOC_PERTURB_=198` injection without
  SIGSEGV; the assertion-density gate continues to pass (net +16 assertions).
- **Negative**: minor verbosity in the three test files.
- **Neutral / follow-ups**: `core/test/AGENTS.md` gains the invariant rule
  that all `malloc` in tests must be NULL-checked.

## References

- req: Round 27 audit D.1 — 10 unchecked `malloc()` sites in 3 test files.
- [ADR-0015](0015-ci-matrix-asan-ubsan-tsan.md) — sanitizer matrix.
- [ADR-0245](0245-simd-bitexact-test-harness.md) — SIMD bit-exact test harness.
- [ADR-0141](0141-touched-file-cleanup-rule.md) — fix preexisting bugs in touched files.
