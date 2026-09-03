<!-- markdownlint-disable MD013 -->
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

- **No dead `/* ... */` blocks in test files.** Commented-out code that cannot
  compile (duplicate declarations, type-mismatched calls, stale APIs) must be
  deleted rather than left in place. If a test scenario is genuinely planned but
  not yet ready, add a `// TODO(ADR-NNNN): <one-line description>` comment
  instead — not a multi-line block comment containing broken code. See
  [ADR-0970](../../docs/adr/0970-test-gpu-picture-pool-cleanup.md) for the
  precedent (Round 27 audit D.4: `test_ring_buffer_threaded` dead block deleted).
- **Every `malloc` / `calloc` in a test must be NULL-checked immediately.**
  Two idiomatic patterns are accepted:
  1. *Consolidated guard* (multi-alloc SIMD tests): allocate all buffers,
     then `if (!a || !b || ...) { free(a); free(b); ...; return "malloc failed"; }`.
     Safe because `free(NULL)` is a no-op. Reference: `test_integer_adm_simd.c`
     (L172–182), `test_vif_simd.c` (L163–170).
  2. *`mu_assert` guard* (loop-local single allocations):
     `mu_assert("malloc failed for X", ptr);` immediately after the call.
     Reference: `test_framesync.c`, `test_pic_preallocation.c`.
  Do **not** dereference a `malloc` return value before checking it.
  An unchecked dereference is a latent SIGSEGV under ASan `MALLOC_PERTURB_=198`
  (ADR-0971). **Rebase-sensitive**: this rule applies to every new test file.
- **Authoritative test twins (ADR-1153)**:
  `test_dict.cpp` and `test_feature.cpp` are the sole authoritative test files
  for `dict` and `feature_name`; the uncompiled legacy C twins `test_dict.c`
  and `test_feature.c` were deleted as obsolete.
- **GPU tests must skip gracefully when no device is present.** Any test that
  calls `vmaf_cuda_state_init`, `vmaf_hip_state_init`, or equivalent GPU-init
  helpers must check the return value before proceeding. On failure (`err != 0`
  or returned pointer is NULL), emit `[skip: no CUDA/HIP/Vulkan device]` to
  stderr and `return NULL` — do not hard-fail via `mu_assert`. Replacing a
  hard-fail `mu_assert` with a skip guard is a one-line pattern; see
  `test_cuda_buffer_alloc_oom.c` and `test_cuda_pic_preallocation.c` for
  reference. **Rebase-sensitive**: any new GPU test that lacks this guard will
  SIGSEGV on CPU-only CI runners.
- **`test_integer_cambi_sycl.c` is a smoke test; `test_sycl_cambi_parity.c` is the
  parity gate.** Both exist intentionally. The smoke test (ADR-0371) verifies
  registration + finite/non-negative output on a flat frame. The parity test
  (ADR-1001, round 5) asserts the headline `Cambi_feature_cambi_score` matches the
  CPU path within places=4 on a banding fixture. Do not merge the two files — they
  serve different audit purposes. Do not remove `test_integer_cambi_sycl.c` in the
  belief that the parity test supersedes it; the registration + format contract it
  pins is a separate invariant. **Rebase-sensitive**: if `integer_cambi_sycl.cpp`
  gains a new feature-name key or option, update both tests.
- **GPU-only extractors get a smoke gate, not a parity gate.** When a CUDA /
  HIP / SYCL feature extractor has no CPU twin emitting the same feature name
  (e.g. `speed_chroma_cuda`, `speed_temporal_cuda` — emit
  `Speed_*_feature_*_score`, no CPU producer), a CPU-vs-GPU parity assertion
  is the wrong tool. The gate is a smoke test: register the extractor, run a
  multi-frame fixture, assert finite scores at frame index 1. Catches the
  high-impact failure modes (NaN/Inf drift from kernel grid changes or
  covariance-matrix degenerate cases) without inventing a redundant CPU
  reference. See `test_cuda_speed_chroma_smoke.c` /
  `test_cuda_speed_temporal_smoke.c` (ADR-0956). Fixture sizing matters here:
  the speed kernels need 640x360+ to admit a non-singular covariance matrix in
  the ADR-0567 host-side eigendecomp path. **Rebase-sensitive**: do not "fix"
  a smoke test by adding a fake CPU twin — the ADR-0956 alternatives table
  documents why.
  calls `vmaf_cuda_state_init`, `vmaf_hip_state_init`, `vmaf_metal_state_init`,
  or equivalent GPU-init helpers must check the return value before proceeding.
  On failure (`err != 0` or returned pointer is NULL), emit
  `[skip: no CUDA/HIP/Metal/Vulkan device]` to stderr and `return NULL` — do
  not hard-fail via `mu_assert`. Replacing a hard-fail `mu_assert` with a skip
  guard is a one-line pattern; see `test_cuda_buffer_alloc_oom.c`,
  `test_cuda_pic_preallocation.c`, `test_sycl_motion3_parity.c`, and the Metal
  parity tests `test_metal_*_parity.c` for reference. **Rebase-sensitive**: any
  new GPU test that lacks this guard will SIGSEGV on CPU-only CI runners (and
  on macOS Intel runners, where Metal returns `-ENODEV`).
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
- **MS-SSIM / `float_ms_ssim` fixture dims must be ≥ 176×176.** The 5-level
  11-tap MS-SSIM pyramid rejects any input where `min(w, h) < GAUSSIAN_LEN
  << (SCALES - 1) = 11 << 4 = 176` at init with `-EINVAL` (see
  `core/src/feature/float_ms_ssim.c:131-138`, Netflix#1414 / ADR-0153).
  A test fixture below this floor will fail at the *first*
  `vmaf_read_pictures` call with `"vmaf_read_pictures failed"`, masking
  the actual code path you intended to test. Use 192×192 or larger (192 =
  176 rounded up to a multiple of 16 for clean pyramid downsamples).
  This caught `test_metal_float_ms_ssim_parity` on all macOS jobs at
  master `4948b771c`; see [ADR-0973](../../docs/adr/0973-master-ci-regressions-verified-2026-05-31.md).
  **Rebase-sensitive**: any new test that exercises `float_ms_ssim` /
  `float_ms_ssim_metal` / `float_ms_ssim_*` (any backend) must use
  fixtures ≥ 176 in both dimensions.
- **SSIMULACRA 2 SIMD test scalar reference is icx-FMA-sensitive.** The
  scalar reference functions in
  [`test_ssimulacra2_simd.c`](test_ssimulacra2_simd.c) (e.g.
  `ref_linear_rgb_to_xyb`) must match the AVX2 / AVX-512 SIMD libs
  bit-for-bit, but those libs use explicit `_mm*_mul_ps` + `_mm*_add_ps`
  intrinsics (no `_mm*_fmadd_ps`). Under icx 2025.3 / 2026.0, neither
  `-ffp-contract=off`, `-fp-model=precise`, nor
  `#pragma STDC FP_CONTRACT OFF` suppresses scalar FMA contraction —
  only **`#pragma clang fp contract(off)`** does. The file carries a
  file-scope clang FP pragma block (with `-Wunknown-pragmas` suppression
  for GCC) at the top; do not remove it. Any new ref function added to
  this file inherits the pragma scope automatically.
  See [ADR-0973](../../docs/adr/0973-master-ci-regressions-verified-2026-05-31.md).
  **Rebase-sensitive**: if a refactor moves the ref functions out into a
  helper header, port the pragma block with them.
- **GPU dispatch-runtime test mutates the process env.**
  [`test_gpu_dispatch_runtime.c`](test_gpu_dispatch_runtime.c) calls
  `setenv()` on `VMAFX_TEST_DISPATCH_RUNTIME_*` keys + the real
  `VMAF_CUDA_DISPATCH` to exercise the once-snapshot semantics. The
  snapshot table is a process-wide singleton (ADR-0461) so the first
  `vmaf_gpu_dispatch_env_get(key)` call wins permanently — tests must
  pre-set the env BEFORE the first selector call. The test executable
  is fork-local (no upstream coupling); namespaced `VMAFX_TEST_*` keys
  prevent collisions with production `VMAF_*_DISPATCH` variables. See
  [ADR-0954](../../docs/adr/0954-gpu-runtime-coverage-test.md).
- **New SIMD parity test → use [`simd_bitexact_test.h`](simd_bitexact_test.h)**
  (ADR-0245). The shared harness centralises the `xorshift32` PRNG,
  the portable POSIX/MinGW/MSVC aligned allocator, the x86 AVX2 CPUID
  gate, and the `SIMD_BITEXACT_ASSERT_MEMCMP` /
  `SIMD_BITEXACT_ASSERT_RELATIVE` assertion macros. Do not re-implement
  these inline. `#include "test.h"` must precede `#include
  "simd_bitexact_test.h"` (conventional order; the previous
  double-include risk was removed when `test.h` gained an include guard
  in this PR). Existing migrated tests
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

## Pixel-format edge coverage invariant (ADR-0912)

`test_pixel_format_edge_coverage.c` is the canonical home for
cross-cutting `(extractor × pix_fmt × bpc)` smoke tests. Five cases
ship today (PSNR on 4:2:2 8-bit, 4:4:4 10-bit, 4:2:0 12-bit; SSIM on
4:2:2 8-bit; CIEDE on 4:2:2 8-bit). When adding a new extractor or
extending an existing one to a previously-unsupported pixel format,
add a follow-up case to this file rather than to a new per-extractor
file — the audit value of one file per cross-cutting axis is higher
than per-extractor locality. The file links only against the **public**
extractor / picture / collector C surface (no internal-source
`#include`); preserve that property so the test stays a regression
gate for the published API. See
[ADR-0912](../../docs/adr/0912-pixel-format-edge-coverage.md).

## libFuzzer harnesses (`fuzz/`)

The [`fuzz/`](fuzz/) subdir holds libFuzzer harnesses for parser
surfaces (ADR-0270 scaffold; ADR-0311 expansion;
[ADR-0882](../../docs/adr/0882-fuzz-target-audit-json-model-sidecar.md)
json_model + dnn_sidecar additions). Conventions:

- Each harness binds **one** public parser entry point via
  `LLVMFuzzerTestOneInput(const uint8_t *, size_t)`. Harnesses
  that need a `FILE *` use `fmemopen`; path-based loaders use a
  per-process `/tmp/vmaf-fuzz-<target>-<pid>` tempfile reused
  across iterations (see `fuzz_dnn_sidecar.c` for the pattern).
- Internal (non-`VMAF_EXPORT`) entry points cannot be reached
  through `libvmaf.so` because
  [ADR-0379](../../docs/adr/0379-libvmaf-symbol-visibility.md)
  builds the shared library with `-fvisibility=hidden`. Mirror
  the precedent in `test_model` / `test_model_loader` and
  compile the relevant source files directly into the harness
  binary (e.g. `fuzz_json_model` pulls
  `core/src/read_json_model.c` + `pdjson.c` + `dict.c` + `log.c`).
- Seed corpora under `<target>_corpus/` are committed verbatim
  and kept small (one per branch class). Known-crash reproducers
  go under `<target>_known_crashes/` and are **excluded** from
  the nightly CI seed path; they exist so the regression catches
  the moment the underlying fix lands.
- Per [ADR-0404](../../docs/adr/0404-nightly-fuzz-triage-keep-gates.md),
  a harness that surfaces a real bug stays on in CI without
  `continue-on-error` until the fix lands. Document the finding
  in `docs/state.md` and link the reproducer from `README.md`.
- The fuzz build requires clang + `-Db_lto=false` when any
  harness pulls libvmaf-internal sources (ASan + LTO discards
  module-dtor sections at link time on the larger source sets).
  See the build recipe at the top of `fuzz/README.md`.
  - **A ported assertion is measured against the merge base, never against
    a baseline you regenerated afterwards.** When ADR-1153 makes you port a
    dead twin's unique coverage into the live side before deleting it, the
    twin's idioms come with it — and if the twin was C and the live side is
    C++, every `NULL`, `typedef struct`, `{0}` sentinel and file-scope
    `static` is a fresh clang-tidy warning. Run
    `python3 scripts/ci/tidy-ratchet.py --lane cpu --build-dir build`
    against the branch's **merge base** and again after the port, and
    compare the two. Regenerating the baseline after the port makes any
    increase invisible: that is how PR #1219 took
    `core/test/test_feature.cpp` from 9 warnings to 34 without the gate
    firing. Translate the idioms as you port — C++ TUs use `nullptr`
    (ADR-1138's `NULL` rule is scoped to **C** TUs, for MSVC
    `/std:clatest`), plain `struct`, `{}` sentinels, and an anonymous
    namespace instead of file-scope `static`.
  - **Free before you assert.** `mu_assert` expands to an early
    `return message`, so any assertion evaluated while a heap pointer is
    live leaks that pointer on the failure path. Compute the comparison
    into a `const bool`, free, then assert on the bool — and null-guard the
    comparison so a `nullptr` return fails the assertion instead of
    faulting inside `strcmp`. `clang-analyzer-unix.Malloc` only reports
    this once a function is small enough for it to analyse fully, so an
    oversized test function hides the leak rather than avoiding it.
  - **`read_json_model` is a twin pair, and the fuzz harness uses the `.c`
    side.** The library builds `core/src/read_json_model.cpp`; the libFuzzer
    target compiles `core/src/read_json_model.c` directly
    (`core/test/fuzz/meson.build`). The pair is not in
    `scripts/ci/twin-drift-allowlist.txt`, so a parser fix must land in
    **both** files. Fixing one and testing the other is a real trap: a
    library-linked reproducer will report the bug fixed while the fuzz lane
    stays red, because the two binaries do not share that translation unit.
    `scripts/ci/twin-drift-check.sh` labels the `.c` a "test-only twin side".
