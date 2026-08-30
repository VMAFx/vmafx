# Research-0090: Sanitizer matrix test-set scope decision

- **Date**: 2026-05-09
- **Author**: Lusoris + Claude (Anthropic)
- **Status**: Companion to [ADR-0347](../adr/0347-sanitizer-matrix-test-scope.md)
- **Tags**: ci, testing, sanitizer

## Question

What test set should the sanitizer matrix
([`tests-and-quality-gates.yml::sanitizers`](../../.github/workflows/tests-and-quality-gates.yml))
run under each of ASan / UBSan / TSan?

The pre-existing matrix runs `meson test --suite=unit`, which
matches zero tests because no `test()` call in
[`core/test/meson.build`](../../core/test/meson.build) carries
a `suite: 'unit'` tag (`grep -nE 'suite\s*[:=]\s*"' core/test/meson.build`
returns no rows). Output: `No suitable tests defined.` →
exit 0 → green check → zero correctness coverage.
[Research-0089 §5](../research/0089-vulkan-vif-fp-residual-bisect-2026-05-08.md)
flagged the symptom but parked the fix as out-of-scope of the
parent CI audit.

## Method

Empirical: build libvmaf three times locally on this branch
(clang-22 stand-in for CI's clang-18; the defect classes
reproduce identically across clang versions because the bugs are
in the library code under test, not the compiler), each time
with one of `b_sanitize=address|undefined|thread`, then run the
full C unit-test set and capture pass/fail per test plus wall-time.

```bash
cd libvmaf

CC=clang CXX=clang++ LDFLAGS=-fuse-ld=lld \
  meson setup build-asan -Db_sanitize=address \
    -Denable_cuda=false -Denable_sycl=false --buildtype=debug \
    -Db_lto=false -Db_lundef=false
meson compile -C build-asan
time meson test -C build-asan --print-errorlogs

CC=clang CXX=clang++ LDFLAGS=-fuse-ld=lld \
  meson setup build-ubsan -Db_sanitize=undefined \
    -Denable_cuda=false -Denable_sycl=false --buildtype=debug \
    -Db_lto=false -Db_lundef=false \
    "-Dc_args=-fno-sanitize=function" "-Dcpp_args=-fno-sanitize=function"
meson compile -C build-ubsan
time meson test -C build-ubsan --print-errorlogs

CC=clang CXX=clang++ LDFLAGS=-fuse-ld=lld \
  meson setup build-tsan -Db_sanitize=thread \
    -Denable_cuda=false -Denable_sycl=false --buildtype=debug \
    -Db_lto=false -Db_lundef=false
meson compile -C build-tsan
time meson test -C build-tsan --print-errorlogs
```

## Findings

### ASan — 47 tests run, 3 fail

| # | Test | Defect site | Defect class |
| --- | --- | --- | --- |
| (a) | `test_model::test_json_model_synthetic_branches` | `core/src/svm.cpp:2955` `parse_support_vectors` | malloc-too-big (`0xfffffffffffffff8` on malformed JSON model buffer; missing length validation in `SVMModelParser`) |
| (b) | `test_predict::test_propagate_metadata` | `core/src/dict.c:124` `dict_append_new_entry` (via `feature_collector_dispatch_metadata`) | direct + indirect leaks (162 bytes / 4 allocations) — metadata `dict` entries + `strdup`'d keys not freed when `vmaf_predict_score_at_index` re-enters the collector |
| (c) | `test_float_ms_ssim_min_dim::test_float_ms_ssim_init_*` | `test/test_float_ms_ssim_min_dim.c:33` `invoke_init` | direct calloc leaks (240 bytes / 6 allocations) — extractor state allocated by `init` never reclaimed by the test bodies |

### UBSan — 49 tests run (with `-fno-sanitize=function`), 1 fail

| # | Test | Defect site | Defect class |
| --- | --- | --- | --- |
| (d) | `test_model::test_json_model_synthetic_branches` | `core/src/svm.cpp:2989` `memcpy` arg2 | NULL passed where `__nonnull` is declared — same parse path as (a), different angle on the same missing-validation bug |

Without `-fno-sanitize=function`, **23 / 50 tests fail** with
`runtime error: call to function test_X through pointer to
incorrect function type 'char *(*)(void)'` because the K&R-prototype
`static char *test_X()` pattern in ~50 minunit-style harness files
(originating in upstream Netflix `core/test/test.h`) trips
UBSan's `function` check. This is harness UB, not library UB; the
fix is to add `(void)` parameter declarations across all 50 test
files (a separate T7-5-style sweep PR — out of scope for this ADR).
The ADR-0347 build flag suppresses *only* the `function` check, so
UBSan still surfaces every signal in library code.

### TSan — 47 tests run, 3 fail

| # | Test | Defect site | Defect class |
| --- | --- | --- | --- |
| (e) | `test_model::test_json_model_synthetic_branches` | same as (a)/(d) | same defect, different sanitizer |
| (f) | `test_pic_preallocation` | `core/src/feature/integer_adm.h:36` `div_lookup_generator` (called from `init` in `integer_adm.c:3360` via `vmaf_thread_pool_runner`) | data race on global `div_lookup` table — write-write race on the same table index from worker threads T1 and T4 (no atomic, no init-once guard) |
| (g) | `test_framesync` | `core/src/framesync.c:125` `vmaf_framesync_submit_filled_data` reads `count` while `framesync.c:102` `vmaf_framesync_acquire_new_buf` writes the same `count` | mutex-domain mismatch — reader holds mutex M0, writer holds mutex M1; the per-buffer state is shared across both critical sections without consistent locking |

### Wall-time per leg

| Leg | Test exec wall (local) | Tests run / total | Notes |
| --- | --- | --- | --- |
| ASan | ~5.3 s | 36 / 36 OK with deselects | Dominated by `test_framesync` (5.0 s) and `test_pic_preallocation` (2.3 s). |
| UBSan | ~5.4 s | 38 / 38 OK with deselects + `-fno-sanitize=function` | Same dominant tail; `test_pic_preallocation` and `test_framesync` are not deselected because they only fail under TSan. |
| TSan | ~1.3 s | 36 / 36 OK with deselects | `test_framesync` and `test_pic_preallocation` are deselected because of the races; the remaining tests run faster under TSan than under ASan because the fastest-failing tests are excluded. |

Build wall is the same as the pre-existing job (~3–4 min per leg);
the change adds only the test-execution time on top.

## Recommendation

Adopt the test-set scope codified in ADR-0347:

- ASan: full unit suite minus `test_model`, `test_predict`,
  `test_float_ms_ssim_min_dim`.
- UBSan: full unit suite minus `test_model`, with
  `-fno-sanitize=function` to suppress the upstream K&R-harness UB.
- TSan: full unit suite minus `test_model`,
  `test_pic_preallocation`, `test_framesync`.
- No MSan leg (existing matrix has always been
  ASan + UBSan + TSan).

The seven defects surfaced (a)–(g) become Open-bugs rows in
[`docs/state.md`](../state.md) so each becomes a tracked
follow-up. As fixes land, the corresponding deselect rows
disappear from the workflow YAML.

## References

- [ADR-0347](../adr/0347-sanitizer-matrix-test-scope.md) (decision).
- [ADR-0015](../adr/0015-ci-matrix-asan-ubsan-tsan.md) (parent —
  matrix established).
- [`.github/workflows/tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml)
  job `sanitizers`.
- Local rerun command in any of the three sanitizer build dirs:

  ```bash
  meson test -C build-<asan|ubsan|tsan> --print-errorlogs
  ```
