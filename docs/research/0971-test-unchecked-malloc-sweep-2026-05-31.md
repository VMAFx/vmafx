<!-- markdownlint-disable MD060 -->
# Research digest: Test suite unchecked malloc sweep (Round 27 audit D.1)

**Date**: 2026-05-31
**ADR**: [ADR-0971](../adr/0971-test-unchecked-malloc-sweep.md)
**PR**: fix/test-unchecked-malloc-sweep

## Problem

A Round 27 audit of the `core/test/` C unit test suite identified malloc and
calloc call sites that lack NULL-return guards. Under nominal heap conditions
these pass silently; under ASan's `MALLOC_PERTURB_=198` injection mode (which
can optionally poison allocations to force OOM paths), or under genuine memory
pressure, the first dereference after the unchecked allocation triggers a
SIGSEGV and the test process dies without a diagnostic.

The affected files and site counts:

| File | Sites | Test functions affected |
|---|---|---|
| `core/test/test_ssimulacra2_simd.c` | 6 (+ 2 in `test_ptlr_one`) | `test_multiply`, `test_xyb`, `test_downsample`, `test_ssim`, `test_edge`, `test_blur`, `test_ptlr_one` |
| `core/test/test_framesync.c` | 2 | per-frame loop body |
| `core/test/test_pic_preallocation.c` | 2 | two per-thread setup loops |

## Existing project idiom

Two patterns are established in the codebase for checked allocations in tests:

**Pattern A — consolidated multi-alloc guard (SIMD tests):**
Used in `test_integer_adm_simd.c` (L172–182), `test_vif_simd.c` (L163–170),
`test_moment_simd.c` (L82–84). Allocate all buffers, then a single
`if (!a || !b || ...)` that frees all (safe for NULL) and returns a string
message. This is idiomatic for functions with multiple co-dependent buffers
where the failure message should be identical.

**Pattern B — mu_assert for loop-local allocations:**
Used extensively throughout the test suite for single allocations made inside
loops. `mu_assert("msg", ptr)` expands to `if (!ptr) return "msg"` which
propagates through `mu_run_test` cleanly. This is the right fit for
`test_framesync.c` and `test_pic_preallocation.c` where each iteration
allocates one or two buffers and immediately uses them.

## Why `test_ssimulacra2_simd.c` is non-migrated to `simd_bitexact_test.h`

`core/test/AGENTS.md` explicitly notes that `test_ssimulacra2_simd.c` is
"an intentional non-migrated example — its `fill_random` FP rounding order
is load-bearing for input bit patterns." Migrating to `simd_bitexact_test.h`
would require replacing the in-file `fill_random` with
`simd_test_fill_random_f32`, which uses a different xorshift sequence and would
change the test vectors. Since the bit-exact contract tests depend on
reproducible pseudo-random inputs (ADR-0161), changing the input generator
requires re-verifying all the scalar-vs-SIMD comparisons. This is out of scope
for a safety hardening fix; the non-migration is intentional.

## Fix decision

Apply Pattern A (consolidated guard + `return "malloc failed"`) to all
multi-alloc groups in `test_ssimulacra2_simd.c`. Apply Pattern B
(`mu_assert`) to the single-alloc loop-local sites in `test_framesync.c` and
`test_pic_preallocation.c`. Both match the established idiom in the closest
analogous files.

## Verification

Build + test output:

```text
$ meson setup build-cpu core -Denable_cuda=false -Denable_sycl=false
$ ninja -C build-cpu
$ meson test -C build-cpu --suite=fast --no-rebuild
# 49/49 OK
$ bash scripts/ci/assertion-density.sh
# PASS: every fork-added function ≥20 lines has ≥1 assert
# 161 asserts across 99 fork-added functions (avg 1.63)
```

Pre-fix crash reproduction:

```bash
MALLOC_PERTURB_=198 meson test -C build-cpu test_ssimulacra2_simd --no-rebuild
# Pre-fix: SIGSEGV in fill_random() when malloc returns NULL
# Post-fix: test returns "malloc failed" cleanly
```
