# AGENTS.md — core/test

Orientation for agents working on the C unit test suite. Parent:
[../AGENTS.md](../AGENTS.md).

## Scope

C unit tests for the libvmaf engine. Runs on every build via
`meson test -C build`. A separate suite under
[dnn/](dnn/) covers the ONNX Runtime integration.

## Test style

All tests follow a trivial µnit-style pattern declared in
[test.h](test.h):

```c
static char *test_some_invariant(void)
{
    mu_assert("description", predicate);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_some_invariant);
    return NULL;
}
```

Each `test_*.c` compiles into its own binary. `meson.build` registers them
with `meson test`. No fixtures, no shared state — each test owns its setup
and teardown.

## Ground rules

- **GPU tests must skip gracefully when no device is present.** Any test that
  calls `vmaf_cuda_state_init`, `vmaf_hip_state_init`, or equivalent GPU-init
  helpers must check the return value before proceeding. On failure (`err != 0`
  or returned pointer is NULL), emit `[skip: no CUDA/HIP/Vulkan device]` to
  stderr and `return NULL` — do not hard-fail via `mu_assert`. Replacing a
  hard-fail `mu_assert` with a skip guard is a one-line pattern; see
  `test_cuda_buffer_alloc_oom.c` and `test_cuda_pic_preallocation.c` for
  reference. **Rebase-sensitive**: any new GPU test that lacks this guard will
  SIGSEGV on CPU-only CI runners.
- **Parent rules** apply (see [../AGENTS.md](../AGENTS.md)).
- **POSIX-only APIs in tests** must be shimmed for MINGW. See
  [test_lpips.c](test_lpips.c) for the `_putenv_s`-based shim for
  `setenv`/`unsetenv` — MinGW's mingw.org / MSYS2 headers do not expose
  those functions under `-std=c11 -pedantic`. The CI MINGW build will catch
  this but running `meson test` locally on Linux won't.
- **Never modify Netflix golden assertions**: those are Python-side, not
  here — see [../../python/test/](../../python/test/) and
  [ADR-0024](../../docs/adr/0024-netflix-golden-preserved.md).
- **New extractor → new test file** following the `test_lpips.c` pattern:
  (a) registered by name, (b) registered by provided feature name,
  (c) options table well-formed, (d) init rejects missing required input.
- **Output / writer-format tests use `tmpfile()` + slurp.**
  [`test_output.c`](test_output.c) is the reference for exercising
  `vmaf_write_output_{xml,json,csv,sub}` (R3 of the
  [coverage gap analysis](../../docs/development/coverage-gap-analysis-2026-05-02.md)):
  open a `tmpfile()`, run the writer, `fseek(SEEK_END)` + `ftell` +
  `fseek(SEEK_SET)` + `fread` to slurp the buffer, then `strstr` for
  expected markers.
- **Tests that need a *named* temp file (path-on-disk dispatch)** must
  resolve the temp directory at runtime — never hardcode `/tmp/...`
  with `mkstemp(3)`. MSYS2/MinGW64 inside the GitHub Actions
  `windows-latest` runner does not expose a usable `/tmp` from the
  `MINGW64` shell, so `mkstemp` against a `/tmp/foo_XXXXXX` template
  fails with `ENOENT` and the test wedges the Windows matrix leg red
  (ADR-0515 history: `test_public_api_score::test_vmaf_write_output`).
  Reference patterns: the `make_temp_output_path()` helper in
  [test_public_api_score.c](test_public_api_score.c) and the inline
  `#ifdef _WIN32 ... GetTempPathA ... #else mkstemp ... #endif` block
  in [dnn/test_model_loader.c](dnn/test_model_loader.c)
  (`test_sidecar_parses`). Both use `<stdio.h>` `remove(path)` instead
  of `unlink(path)` so `<unistd.h>` doesn't have to be pulled in on
  Windows. To reach `vmaf_feature_score_pooled` the test must
  use a real `VmafContext` (the writers require it for the
  `pooled_metrics` block); obtain the owned collector through
  `core/src/libvmaf_priv.h::vmaf_feature_collector_get()` and link
  against libvmaf. Do not include `libvmaf.c` / `output.c` directly from
  `test_output.c`: Apple ld64 + LTO has resolved that duplicate-definition
  pattern incorrectly under allocator poisoning, causing macOS writer-test
  SIGSEGVs. `test_output` still needs private-symbol access, so its Meson
  target disables LTO on Darwin only; Linux clang must keep LTO enabled at
  link time because `src/libvmaf.a` contains LLVM bitcode in clang builds.
  Public ABI tests that do not need private symbols must link
  `libvmaf_public_link` so `default_library=both` exercises the shared library
  instead of Apple ld64's static-LTO path. **Pooled-metrics invariant**:
  for the writer to emit per-feature mean/min/max/harmonic_mean
  entries, *every* index in `[0, pic_cnt)` must have a written value
  for every feature — `vmaf_feature_score_pooled` returns `-EAGAIN`
  on the first missing index and the writer skips that feature.
  Sparse-frame branches (`count_written_at == 0`, `i > capacity`)
  belong in CSV / SUB tests where pic_cnt isn't a precondition.
- **New SIMD parity test → use [`simd_bitexact_test.h`](simd_bitexact_test.h)**
  (ADR-0245). The shared harness centralises the `xorshift32` PRNG,
  the portable POSIX/MinGW/MSVC aligned allocator, the x86 AVX2 CPUID
  gate, and the `SIMD_BITEXACT_ASSERT_MEMCMP` /
  `SIMD_BITEXACT_ASSERT_RELATIVE` assertion macros. Do not re-implement
  these inline. **Include-order invariant**: `#include "test.h"` MUST
  precede `#include "simd_bitexact_test.h"` because `test.h` has no
  header guard and would redefine the `mu_report` `static inline` on a
  double include. Existing migrated tests
  (`test_psnr_hvs_avx2.c`, `test_psnr_hvs_neon.c`, `test_moment_simd.c`,
  `test_motion_v2_simd.c`) are reference templates;
  `test_ssimulacra2_simd.c` is an intentional non-migrated example
  (its `fill_random` FP rounding order is load-bearing for input bit
  patterns).

## Governing ADRs

- [ADR-0015](../../docs/adr/0015-ci-matrix-asan-ubsan-tsan.md) — sanitizer
  matrix (tests run under ASan + UBSan + TSan).
- [ADR-0024](../../docs/adr/0024-netflix-golden-preserved.md) — Netflix
  goldens (Python-side) never change.
- [ADR-0245](../../docs/adr/0245-simd-bitexact-test-harness.md) — SIMD
  bit-exact test harness shared header (`simd_bitexact_test.h`).
- [ADR-0515](../../docs/adr/0515-test-public-api-score-mingw64-temp-path.md) —
  MinGW64 portable temp-path: no hardcoded `/tmp/` + `mkstemp`; use
  `make_temp_output_path()` pattern (`GetTempPathA` on `_WIN32`, `mkstemp` on
  POSIX). **Rebase-sensitive**: any new test that needs a named temp file must
  follow this pattern or it will wedge the `Build — Windows MinGW64 (CPU)` leg.
- [ADR-0521](../../docs/adr/0521-msvc-posix-gating-vif-avx512-yuv-input.md) —
  MSVC portability: C source files touched by agents must not use bare
  `__attribute__((noinline, noclone))` without a MSVC-guarded macro, and must
  not call `fstat()` / `S_ISREG()` / rely on `off_t` being 64-bit without the
  `#ifdef _WIN32` shims established in `core/tools/yuv_input.c`. **Rebase-
  sensitive**: any new `.c` file that introduces GCC-extension attributes or
  POSIX `<sys/stat.h>` calls must add a matching portability guard or it will
  wedge `Build — Windows MSVC + CUDA` and `Build — Windows MSVC + oneAPI SYCL`.
- [ADR-0347](../../docs/adr/0347-sanitizer-matrix-test-scope.md) —
  sanitizer matrix test-set scope. **Rebase-sensitive invariant**:
  the sanitizer job in
  `.github/workflows/tests-and-quality-gates.yml` enumerates the
  full unit-test set via `meson test --list` and applies a
  per-sanitizer deselect regex (ASan / UBSan / TSan each have
  their own list). When adding a new `test()` call to
  [`meson.build`](meson.build), the test inherits sanitizer
  coverage automatically. Do NOT add a `suite: 'unit'` tag to
  any `test()` call without coordinating with ADR-0347 — the
  workflow no longer relies on `--suite=unit` (which previously
  matched zero tests because no `test()` carried the tag) and
  partial tagging would silently re-introduce the gap. Under
  UBSan the build adds `-fno-sanitize=function` to suppress the
  K&R-prototype harness UB across every `test_*.c`; new test
  files should follow the existing `static char *test_X()`
  pattern for upstream-parity. A future T7-5-style sweep PR
  that converts every test function to `(void)` parameters
  must also drop `-fno-sanitize=function` from the workflow in
  the same PR.

## Suite-tagging invariant

**Every `test()` declaration in [`meson.build`](meson.build) MUST carry a
`suite:` argument.** The `fast` suite is the documented pre-push gate
(`CLAUDE.md §3`; `meson test -C build --suite=fast`) and must contain every
test that completes in under 2 seconds under normal CPU load.

Tag assignments:

| Suite tag(s)          | Criteria                                                  |
|-----------------------|-----------------------------------------------------------|
| `['fast']`            | CPU-only unit test, finishes in <2 s                      |
| `['fast', 'simd']`    | SIMD bit-exactness test, arch-gated, finishes in <2 s     |
| `['fast', 'gpu']`     | GPU backend scaffold/contract smoke, finishes in <2 s     |
| `['slow']`            | Runs longer than 2 s (e.g. `test_mcp_smoke`, timeout 60s) |

**Rebase-sensitive**: upstream Netflix/vmaf may add new `test()` calls
without `suite:` arguments when cherry-picking or syncing. After every
upstream sync or port-upstream-commit, run:

```bash
grep "^test(" core/test/meson.build | grep -v "suite :"
```

Any line returned is a violation — add the appropriate `suite:` before
merging. See the audit that identified this bug:
`.workingdir/audit-build-matrix-symbols-2026-05-16.md` finding 5c.
