<!-- markdownlint-disable MD013 -->
# AGENTS.md — core/test

Orientation for agents working on C unit test suite. Parent:
[../AGENTS.md](../AGENTS.md).

## Scope

C unit tests for libvmaf engine. Runs on every build via
`meson test -C build`. Separate suite under
[dnn/](dnn/) covers ONNX Runtime integration.

## Test style

All tests follow trivial µnit-style pattern declared in
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

Each `test_*.c` compiles into own binary. `meson.build` registers them
with `meson test`. No fixtures, no shared state — each test owns setup
and teardown.

**Function size** (`readability-function-size`, 15-branch budget):

- `mu_assert` / `mu_run_test` = 2 branches each (`if` + `do { } while (0)`) -> more than 7 in one function fails.
- more than 7 tests -> `MU_TEST(fn)` rows + `mu_run_table()` from [mu_table.h](mu_table.h). 0 branches at any length.
- assertion-heavy test -> `check_*` helpers, called as `char *msg = check_x(...); if (msg) return msg;` = 1 branch.

**Clean up before asserting:**

- failing `mu_assert` returns at once -> live heap, `FILE`, temp file or changed env var leaks.
- free, close, remove, restore env first; keep result in local; assert last.
- `VMAF_TINY_MODEL_DIR` left set -> every later test in binary breaks.
- Tidy Changed analyzer reports these paths (`clang-analyzer-unix.Malloc`, `clang-analyzer-unix.Stream`).

## Ground rules

- **No dead `/* ... */` blocks in test files.** Commented-out code
  that cannot compile (duplicate declarations, type-mismatched calls,
  stale APIs) must be deleted rather than left in place. If test
  scenario genuinely planned but not yet ready, add
  `// TODO(ADR-NNNN): <one-line description>` comment instead — not
  multi-line block comment containing broken code. See
  [ADR-0970](../../docs/adr/0970-test-gpu-picture-pool-cleanup.md)
  for precedent (Round 27 audit D.4: `test_ring_buffer_threaded` dead
  block deleted).
- **Every `malloc` / `calloc` in test must be NULL-checked
  immediately.** Two idiomatic patterns accepted:
  1. *Consolidated guard* (multi-alloc SIMD tests): allocate all
     buffers, then
     `if (!a || !b || ...) { free(a); free(b); ...; return "malloc failed"; }`.
     Safe because `free(NULL)` is no-op. Reference:
     `test_integer_adm_simd.c` (L172–182), `test_vif_simd.c`
     (L163–170).
  2. *`mu_assert` guard* (loop-local single allocations):
     `mu_assert("malloc failed for X", ptr);` immediately after call.
     Reference: `test_framesync.c`, `test_pic_preallocation.c`.
  Do **not** dereference `malloc` return value before checking it.
  Unchecked dereference is latent SIGSEGV under ASan
  `MALLOC_PERTURB_=198` (ADR-0971). **Rebase-sensitive**: this rule
  applies to every new test file.
- **Framesync initializer failures use a separately compiled test object.**
  `test_framesync_init_failure_impl` compiles `framesync.c` with its four
  init/destroy entry points mapped to test-owned wrapper symbols. Keep the test
  source as an ordinary translation unit: do not include executable `.c` files,
  add a production hook, or replace the deterministic wrappers with a
  platform-specific linker interposer. The test must cover each initialization
  stage, a null init-output pointer, the documented null destroy no-op,
  unpublished context, and exact partial unwind.
- **SIMD parity tests check what kernel leaves outside its output, not only
  what it writes inside.** Kernel storing past its region passes region-only
  comparison. Netflix/vmaf `03b5562c5` + `ea012e387` both that shape, unnoticed
  here ([Research-2063](../../docs/research/2063-upstream-sync-2026-09-adm-vif-simd.md)).
  Allocate output planes at production stride, add trailing slack, fill with
  `simd_test_guard_fill()` from `simd_bitexact_test.h`, assert
  `SIMD_GUARD_ASSERT_UNTOUCHED(simd_test_guard_count_outside(plane, rect), ...)`
  after call. Inputs + outputs share buffer -> byte-compare whole allocation
  against scalar run instead (`test_vif_neon.c`,
  `test_integer_vif_avx2_stages.c`). Sweep small geometries both sides of each
  vector stride, down to extractor minimum of 17. ADM + VIF parity tests =
  reference.
- **Authoritative test twins (ADR-1153)**: `test_dict.cpp` and
  `test_feature.cpp` are sole authoritative test files for `dict`
  and `feature_name`; uncompiled legacy C twins `test_dict.c` and
  `test_feature.c` were deleted as obsolete.
- **CAMBI bounded-search regression seam**: `test_cambi.c` deliberately
  includes the production `cambi.c` translation unit. Keep the tests for TVI
  threshold/difference extremes, an unreachable VLT threshold, and duplicate
  plus descending quick-select inputs. They pin termination bounds and
  partition ordering directly; an end-to-end score alone cannot distinguish
  a hang from a numerically wrong search result.
- **GPU tests must skip gracefully when no device present.** Any
  test calling `vmaf_cuda_state_init`, `vmaf_hip_state_init`, or
  equivalent GPU-init helpers must check return value before
  proceeding. On failure (`err != 0` or returned pointer is NULL),
  emit `[skip: no CUDA/HIP/Vulkan device]` to stderr and
  `return NULL` — do not hard-fail via `mu_assert`. Replacing
  hard-fail `mu_assert` with skip guard is one-line pattern; see
  `test_cuda_buffer_alloc_oom.c` and `test_cuda_pic_preallocation.c`
  for reference. **Rebase-sensitive**: any new GPU test lacking this
  guard will SIGSEGV on CPU-only CI runners.
- **`test_integer_cambi_sycl.c` is smoke test;
  `test_sycl_cambi_parity.c` is parity gate.** Both exist
  intentionally. Smoke test (ADR-0371) verifies registration +
  finite/non-negative output on flat frame. Parity test (ADR-1001,
  round 5) asserts headline `Cambi_feature_cambi_score` matches CPU
  path within places=4 on banding fixture. Do not merge two files —
  they serve different audit purposes. Do not remove
  `test_integer_cambi_sycl.c` in belief that parity test supersedes
  it; registration + format contract it pins is separate invariant.
  **Rebase-sensitive**: if `integer_cambi_sycl.cpp` gains new
  feature-name key or option, update both tests.
- **GPU-only extractors get smoke gate, not parity gate.** When CUDA
  / HIP / SYCL feature extractor has no CPU twin emitting same
  feature name (e.g. `speed_chroma_cuda`, `speed_temporal_cuda` —
  emit `Speed_*_feature_*_score`, no CPU producer), CPU-vs-GPU parity
  assertion is wrong tool. Gate is smoke test: register extractor,
  run multi-frame fixture, assert finite scores at frame index 1.
  Catches high-impact failure modes (NaN/Inf drift from kernel grid
  changes or covariance-matrix degenerate cases) without inventing
  redundant CPU reference. See `test_cuda_speed_chroma_smoke.c` /
  `test_cuda_speed_temporal_smoke.c` (ADR-0956). Fixture sizing
  matters here: speed kernels need 640x360+ to admit non-singular
  covariance matrix in ADR-0567 host-side eigendecomp path.
  **Rebase-sensitive**: do not "fix" smoke test by adding fake CPU
  twin — ADR-0956 alternatives table documents why.
  calls `vmaf_cuda_state_init`, `vmaf_hip_state_init`,
  `vmaf_metal_state_init`, or equivalent GPU-init helpers must check
  return value before proceeding. On failure (`err != 0` or returned
  pointer is NULL), emit `[skip: no CUDA/HIP/Metal/Vulkan device]` to
  stderr and `return NULL` — do not hard-fail via `mu_assert`.
  Replacing hard-fail `mu_assert` with skip guard is one-line
  pattern; see `test_cuda_buffer_alloc_oom.c`,
  `test_cuda_pic_preallocation.c`, `test_sycl_motion3_parity.c`, and
  Metal parity tests `test_metal_*_parity.c` for reference.
  **Rebase-sensitive**: any new GPU test lacking this guard will
  SIGSEGV on CPU-only CI runners (and on macOS Intel runners, where
  Metal returns `-ENODEV`).
- **Parent rules** apply (see [../AGENTS.md](../AGENTS.md)).
- **POSIX-only APIs in tests** must be shimmed for MINGW. See
  [test_lpips.c](test_lpips.c) for `_putenv_s`-based shim for
  `setenv`/`unsetenv` — MinGW's mingw.org / MSYS2 headers do not
  expose those functions under `-std=c11 -pedantic`. CI MINGW build
  will catch this but running `meson test` locally on Linux won't.
- **Never modify Netflix golden assertions**: those are Python-side, not
  here — see [../../python/test/](../../python/test/) and
  [ADR-0024](../../docs/adr/0024-netflix-golden-preserved.md).
- **New extractor → new test file** following `test_lpips.c` pattern:
  (a) registered by name, (b) registered by provided feature name,
  (c) options table well-formed, (d) init rejects missing required input.
- **Output / writer-format tests use `tmpfile()` + slurp.**
  [`test_output.c`](test_output.c) is reference for exercising
  `vmaf_write_output_{xml,json,csv,sub}` (R3 of
  [coverage gap analysis](../../docs/development/coverage-gap-analysis-2026-05-02.md)):
  open `tmpfile()`, run writer, `fseek(SEEK_END)` + `ftell` +
  `fseek(SEEK_SET)` + `fread` to slurp buffer, then `strstr` for
  expected markers.
- **Tests that need *named* temp file (path-on-disk dispatch)** must
  resolve temp directory at runtime — never hardcode `/tmp/...`
  with `mkstemp(3)`. MSYS2/MinGW64 inside GitHub Actions
  `windows-latest` runner does not expose usable `/tmp` from
  `MINGW64` shell. `mkstemp` against `/tmp/foo_XXXXXX` template
  fails with `ENOENT`; test wedges Windows matrix leg red
  (ADR-0515 history: `test_public_api_score::test_vmaf_write_output`).
  Reference patterns: `make_temp_output_path()` helper in
  [test_public_api_score.c](test_public_api_score.c) and inline
  `#ifdef _WIN32 ... GetTempPathA ... #else mkstemp ... #endif` block
  in [dnn/test_model_loader.c](dnn/test_model_loader.c)
  (`test_sidecar_parses`). Both use `<stdio.h>` `remove(path)`
  instead of `unlink(path)` so `<unistd.h>` doesn't have to be
  pulled in on Windows. To reach `vmaf_feature_score_pooled`, test
  must use real `VmafContext` (writers require it for
  `pooled_metrics` block); obtain owned collector through
  `core/src/libvmaf_priv.h::vmaf_feature_collector_get()` and link
  against libvmaf. Do not include `libvmaf.c` / `output.c` directly
  from `test_output.c`: Apple ld64 + LTO has resolved that
  duplicate-definition pattern incorrectly under allocator
  poisoning, causing macOS writer-test SIGSEGVs. `test_output` still
  needs private-symbol access, so its Meson target disables LTO on
  Darwin only; Linux clang must keep LTO enabled at link time
  because `src/libvmaf.a` contains LLVM bitcode in clang builds.
  Public ABI tests that do not need private symbols must link
  `libvmaf_public_link` so `default_library=both` exercises shared
  library instead of Apple ld64's static-LTO path.
  **Pooled-metrics invariant**: for writer to emit per-feature
  mean/min/max/harmonic_mean entries, *every* index in
  `[0, pic_cnt)` must have written value for every feature.
  `vmaf_feature_score_pooled` returns `-EAGAIN` on first missing
  index; writer skips that feature. Sparse-frame branches
  (`count_written_at == 0`, `i > capacity`) belong in CSV / SUB
  tests where pic_cnt isn't precondition.
- **MS-SSIM / `float_ms_ssim` fixture dims must be ≥ 176×176.**
  5-level 11-tap MS-SSIM pyramid rejects any input where
  `min(w, h) < GAUSSIAN_LEN
  << (SCALES - 1) = 11 << 4 = 176` at init with `-EINVAL` (see
  `core/src/feature/float_ms_ssim.c:131-138`,
  Netflix#1414 / ADR-0153). Test fixture below this floor will fail
  at *first* `vmaf_read_pictures` call with
  `"vmaf_read_pictures failed"`, masking actual code path you
  intended to test. Use 192×192 or larger (192 = 176 rounded up to
  multiple of 16 for clean pyramid downsamples). This caught
  `test_metal_float_ms_ssim_parity` on all macOS jobs at master
  `4948b771c`; see
  [ADR-0973](../../docs/adr/0973-master-ci-regressions-verified-2026-05-31.md).
  **Rebase-sensitive**: any new test that exercises `float_ms_ssim` /
  `float_ms_ssim_metal` / `float_ms_ssim_*` (any backend) must use
  fixtures ≥ 176 in both dimensions.
- **SSIMULACRA 2 SIMD test scalar reference is icx-FMA-sensitive.**
  Scalar reference functions in
  [`test_ssimulacra2_simd.c`](test_ssimulacra2_simd.c) (e.g.
  `ref_linear_rgb_to_xyb`) must match AVX2 / AVX-512 SIMD libs
  bit-for-bit, but those libs use explicit `_mm*_mul_ps` +
  `_mm*_add_ps` intrinsics (no `_mm*_fmadd_ps`). Under icx 2025.3 /
  2026.0, neither `-ffp-contract=off`, `-fp-model=precise`, nor
  `#pragma STDC FP_CONTRACT OFF` suppresses scalar FMA contraction —
  only **`#pragma clang fp contract(off)`** does. File carries
  file-scope clang FP pragma block (with `-Wunknown-pragmas`
  suppression for GCC) at top; do not remove it. Any new ref
  function added to this file inherits pragma scope automatically.
  See
  [ADR-0973](../../docs/adr/0973-master-ci-regressions-verified-2026-05-31.md).
  **Rebase-sensitive**: if refactor moves ref functions out into
  helper header, port pragma block with them.
- **GPU dispatch-runtime test mutates process env.**
  [`test_gpu_dispatch_runtime.c`](test_gpu_dispatch_runtime.c) calls
  `setenv()` on `VMAFX_TEST_DISPATCH_RUNTIME_*` keys + real
  `VMAF_CUDA_DISPATCH` to exercise once-snapshot semantics. Snapshot
  table is process-wide singleton (ADR-0488) so first
  `vmaf_gpu_dispatch_env_get(key)` call wins permanently — tests must
  pre-set env BEFORE first selector call. Test executable is
  fork-local (no upstream coupling); namespaced `VMAFX_TEST_*` keys
  prevent collisions with production `VMAF_*_DISPATCH` variables. See
  [ADR-0954](../../docs/adr/0954-gpu-runtime-coverage-test.md).
- **New SIMD parity test → use [`simd_bitexact_test.h`](simd_bitexact_test.h)**
  (ADR-0245). Shared harness centralises `xorshift32` PRNG, portable
  POSIX/MinGW/MSVC aligned allocator, x86 AVX2 CPUID gate, and
  `SIMD_BITEXACT_ASSERT_MEMCMP` / `SIMD_BITEXACT_ASSERT_RELATIVE`
  assertion macros. Do not re-implement these inline.
  `#include "test.h"` must precede
  `#include "simd_bitexact_test.h"` (conventional order; previous
  double-include risk removed when `test.h` gained include guard in
  this PR). Existing migrated tests (`test_psnr_hvs_avx2.c`,
  `test_psnr_hvs_neon.c`, `test_moment_simd.c`,
  `test_motion_v2_simd.c`) are reference templates;
  `test_ssimulacra2_simd.c` is intentional non-migrated example (its
  `fill_random` FP rounding order is load-bearing for input bit
  patterns).

## AArch64-gated tests compile under MSVC (ADR-1260)

`Windows ARM64 MSVC` lane builds every test `core/test/meson.build` gates on
`cpu_family()` containing `aarch64` with `cl.exe`, then runs `--suite fast`.
Rules for those files:

- No POSIX-only headers or calls under `#if ARCH_AARCH64`: `<sys/mman.h>`,
  `<unistd.h>`, `sigaction`, `sigsetjmp`, `sysconf`, `_exit`. Windows twin or
  no use.
- Guard-page probes go through `test_ciede_neon.c`'s five entry points
  (`probe_page_size`, `guarded_row_alloc`, `guarded_row_free`,
  `fault_trap_install` / `fault_trap_restore`, `run_kernel_guarded`): POSIX =
  `mmap` + `PROT_NONE` + `sigsetjmp`; Windows = `VirtualAlloc` +
  `PAGE_NOACCESS` + SEH `__try` / `__except`. Copy that shape, do not
  reinvent.
- ADR-1138 `NULL` carve-out applies (MSVC `/std:clatest`, no `nullptr`).
- Local check before push: `meson setup build/aarch64 core --cross-file
  ~/.cache/vmafx-cross/aarch64-clang.ini`, `meson test -C build/aarch64
  <test>` under qemu. MSVC itself: CI only.

## Governing ADRs

- [ADR-0015](../../docs/adr/0015-ci-matrix-asan-ubsan-tsan.md) —
  sanitizer matrix (tests run under ASan + UBSan + TSan).
- [ADR-0024](../../docs/adr/0024-netflix-golden-preserved.md) —
  Netflix goldens (Python-side) never change.
- [ADR-0245](../../docs/adr/0245-simd-bitexact-test-harness.md) —
  SIMD bit-exact test harness shared header
  (`simd_bitexact_test.h`).
- [ADR-0515](../../docs/adr/0515-test-public-api-score-mingw64-temp-path.md) —
  MinGW64 portable temp-path: no hardcoded `/tmp/` + `mkstemp`; use
  `make_temp_output_path()` pattern (`GetTempPathA` on `_WIN32`,
  `mkstemp` on POSIX). **Rebase-sensitive**: any new test that needs
  named temp file must follow this pattern or it will wedge
  `Build — Windows MinGW64 (CPU)` leg.
- [ADR-0521](../../docs/adr/0521-msvc-posix-gating-vif-avx512-yuv-input.md) —
  MSVC portability: C source files touched by agents must not use
  bare `__attribute__((noinline, noclone))` without MSVC-guarded
  macro, and must not call `fstat()` / `S_ISREG()` / rely on
  `off_t` being 64-bit without `#ifdef _WIN32` shims established in
  `core/tools/yuv_input.c`. **Rebase-sensitive**: any new `.c` file
  that introduces GCC-extension attributes or POSIX
  `<sys/stat.h>` calls must add matching portability guard or it
  will wedge `Build — Windows MSVC + CUDA` and
  `Build — Windows MSVC + oneAPI SYCL`.
- [ADR-0347](../../docs/adr/0347-sanitizer-matrix-test-scope.md) —
  sanitizer matrix test-set scope. **Rebase-sensitive invariant**:
  sanitizer job in
  `.github/workflows/tests-and-quality-gates.yml` enumerates full
  unit-test set via `meson test --list` and applies per-sanitizer
  deselect regex (ASan / UBSan / TSan each have own list). When
  adding new `test()` call to [`meson.build`](meson.build), test
  inherits sanitizer coverage automatically. Do NOT add
  `suite: 'unit'` tag to any `test()` call without coordinating
  with ADR-0347. Workflow no longer relies on `--suite=unit` (which
  previously matched zero tests because no `test()` carried the
  tag); partial tagging would silently re-introduce gap. Under
  UBSan, build adds `-fno-sanitize=function` to suppress
  K&R-prototype harness UB across every `test_*.c`; new test files
  should follow existing `static char *test_X()` pattern for
  upstream-parity. Future T7-5-style sweep PR that converts every
  test function to `(void)` parameters must also drop
  `-fno-sanitize=function` from workflow in same PR.

## Suite-tagging invariant

**Every `test()` declaration in [`meson.build`](meson.build) MUST
carry `suite:` argument.** `fast` suite is documented pre-push gate
(`CLAUDE.md §3`; `meson test -C build --suite=fast`) and must
contain every test that completes in under 2 seconds under normal
CPU load.

Tag assignments:

| Suite tag(s)          | Criteria                                                  |
|-----------------------|-----------------------------------------------------------|
| `['fast']`            | CPU-only unit test, finishes in <2 s                      |
| `['fast', 'simd']`    | SIMD bit-exactness test, arch-gated, finishes in <2 s     |
| `['fast', 'gpu']`     | GPU backend scaffold/contract smoke, finishes in <2 s     |
| `['slow']`            | Runs longer than 2 s (e.g. `test_mcp_smoke`, timeout 60s) |

**Rebase-sensitive**: upstream Netflix/vmaf may add new `test()`
calls without `suite:` arguments when cherry-picking or syncing.
After every upstream sync or port-upstream-commit, run:

```bash
grep "^test(" core/test/meson.build | grep -v "suite :"
```

Any line returned is violation — add appropriate `suite:` before
merging. See audit that identified this bug:
`.workingdir/audit-build-matrix-symbols-2026-05-16.md` finding 5c.

## Pixel-format edge coverage invariant (ADR-0912)

`test_pixel_format_edge_coverage.c` is canonical home for
cross-cutting `(extractor × pix_fmt × bpc)` smoke tests. Five cases
ship today (PSNR on 4:2:2 8-bit, 4:4:4 10-bit, 4:2:0 12-bit; SSIM on
4:2:2 8-bit; CIEDE on 4:2:2 8-bit). When adding new extractor, or
extending existing one to previously-unsupported pixel format: add
follow-up case to this file rather than to new per-extractor file.
Audit value of one file per cross-cutting axis is higher than
per-extractor locality. File links only against **public** extractor
/ picture / collector C surface (no internal-source `#include`);
preserve that property so test stays regression gate for published
API. See
[ADR-0912](../../docs/adr/0912-pixel-format-edge-coverage.md).

## libFuzzer harnesses (`fuzz/`)

[`fuzz/`](fuzz/) subdir holds libFuzzer harnesses for parser
surfaces (ADR-0270 scaffold; ADR-0311 expansion;
[ADR-0882](../../docs/adr/0882-fuzz-target-audit-json-model-sidecar.md)
json_model + dnn_sidecar additions). Conventions:

- Each harness binds **one** public parser entry point via
  `LLVMFuzzerTestOneInput(const uint8_t *, size_t)`. Harnesses
  that need `FILE *` use `fmemopen`; path-based loaders use
  per-process `/tmp/vmaf-fuzz-<target>-<pid>` tempfile reused
  across iterations (see `fuzz_dnn_sidecar.c` for pattern).
- Internal (non-`VMAF_EXPORT`) entry points cannot be reached
  through `libvmaf.so` because
  [ADR-0379](../../docs/adr/0379-libvmaf-symbol-visibility.md)
  builds shared library with `-fvisibility=hidden`. Mirror
  precedent in `test_model` / `test_model_loader` and compile
  relevant source files directly into harness binary (e.g.
  `fuzz_json_model` pulls `core/src/read_json_model.c` +
  `pdjson.c` + `dict.c` + `log.c`).
- Seed corpora under `<target>_corpus/` are committed verbatim
  and kept small (one per branch class). Known-crash reproducers
  go under `<target>_known_crashes/` and are **excluded** from
  nightly CI seed path; they exist so regression catches moment
  underlying fix lands.
- Per [ADR-0404](../../docs/adr/0404-nightly-fuzz-triage-keep-gates.md),
  harness that surfaces real bug stays on in CI without
  `continue-on-error` until fix lands. Document finding in
  `docs/state.md` and link reproducer from `README.md`.
- Fuzz build requires clang + `-Db_lto=false` when any harness
  pulls libvmaf-internal sources (ASan + LTO discards
  module-dtor sections at link time on larger source sets). See
  build recipe at top of `fuzz/README.md`.
  - **Ported assertion is measured against merge base, never
    against baseline you regenerated afterwards.** When ADR-1153
    makes you port dead twin's unique coverage into live side before
    deleting it. Twin's idioms come with it. If twin was C and live
    side is C++, every `NULL`, `typedef struct`, `{0}` sentinel and
    file-scope `static` is fresh clang-tidy warning. Run
    `python3 scripts/ci/tidy-ratchet.py --lane cpu --build-dir build`
    against branch's **merge base** and again after port, compare
    two. Regenerating baseline after port makes any increase
    invisible: that is how PR #1219 took `core/test/test_feature.cpp`
    from 9 warnings to 34 without gate firing. Translate idioms as
    you port — C++ TUs use `nullptr` (ADR-1138's `NULL` rule is
    scoped to **C** TUs, for MSVC `/std:clatest`), plain `struct`,
    `{}` sentinels, and anonymous namespace instead of file-scope
    `static`.
  - **Free before you assert.** `mu_assert` expands to early
    `return message`, so any assertion evaluated while heap pointer
    live leaks that pointer on failure path. Compute comparison into
    `const bool`, free, then assert on bool — null-guard comparison
    so `nullptr` return fails assertion instead of faulting inside
    `strcmp`. `clang-analyzer-unix.Malloc` only reports this once
    function is small enough for it to analyse fully, so oversized
    test function hides leak rather than avoiding it.
  - **`read_json_model` is twin pair, and fuzz harness uses `.c`
    side.** Library builds `core/src/read_json_model.cpp`;
    libFuzzer target compiles `core/src/read_json_model.c` directly
    (`core/test/fuzz/meson.build`). Pair is not in
    `scripts/ci/twin-drift-allowlist.txt`, so parser fix must land
    in **both** files. Fixing one and testing other is real trap:
    library-linked reproducer will report bug fixed while fuzz lane
    stays red, because two binaries do not share that translation
    unit. `scripts/ci/twin-drift-check.sh` labels `.c` "test-only
    twin side".

## New C test files inherit the ADR-1138 `NULL` carve-out (ADR-1166)

`core/test/*.c` compile on Windows MSVC legs with `cl.exe`, whose
documented `/std:clatest` C23 feature set does not include `nullptr`
keyword. C test files spell null pointer constant `NULL` and carry
file-scoped `NOLINTBEGIN/END(modernize-use-nullptr)` bracket citing
ADR-1138 — same shape `core/src/feature/float_motion.c` uses.
Keep closing `NOLINTEND` at EOF when appending to such file;
clang-tidy ratchet counts uncited `NOLINT` as debt (ADR-1142), so
citation comment is part of suppression, not nicety.

`run_tests()` is bounded by `readability-function-size` at 15
branches, and every `mu_run_test` expansion contributes two. Past ~7
cases, group them into named driver functions (see
`test_motion_min_dim.c`'s `run_integer_motion_tests` /
`run_float_and_metal_motion_tests`) rather than adding NOLINT.

## Test timeouts and readiness invariants

- **Timeouts are evidence-based**: test timeouts in `meson.build`
  must reflect measured execution distributions. Timeout may change
  ONLY with measured evidence of why passing case needs it. If test
  times out, investigate root cause (deadlock, socket accept hang,
  blocking I/O) rather than reflexively raising timeout.
- **Readiness is polled, never slept**. Applies when testing
  asynchronous servers or worker threads (such as stdio, UDS, or
  SSE MCP transports). Synchronize on real readiness signals, or
  poll readiness endpoints with timeout, rather than using fixed
  `sleep()` calls.

## Observation-only SVM test cleanup (Research-2049)

`test_svm_parser.c` keeps nine malformed-model fixtures and their
order; its header-size/header-order driver helpers propagate first
failure without adding test count. Parser model views and runtime
API query arrays are read-only. Preserve all assertions, public
`svm.h` calls and ownership teardown; these tests do not justify
changes to vendored `svm.cpp` or its header. See
[Research-2049](../../docs/research/2049-svm-observation-test-lint-2026-09-08.md).

## SpEED test fixture grouping (Research-2050)

`test_speed.c` and `test_speed_qa.c` keep their original five
registrations apiece, assertion expressions/messages, input
literals and API call order. Temporal SpEED-QA setup uses
`alloc_temporal_pictures` and `init_temporal_extractor` to remain
below strict branch limit; each caller must immediately return
helper's failure message. These helpers are setup stages, not
additional registered tests. Descriptor views are const because
context-creation API already accepts read-only descriptors. Preserve
ADR-1138 C `NULL` bracket and measured zero warning baseline. See
[Research-2050](../../docs/research/2050-speed-test-native-lint.md).

## IQA/motion observation fixtures (Research-2053)

`test_iqa_convolve_coverage.c` keeps seven input-only image arrays
const; `iqa_img_filter` inputs and kernel storage remain writable.
Its boundary-test group preserves first five cases, propagates
failure immediately and does not increment test count itself. Five
edge-16 motion source arrays are read-only; all expected sums and
mirror fixtures remain unchanged. See
[Research-2053](../../docs/research/2053-observation-fixture-const-2026-09-08.md).

## Metric coverage setup stages (Research-2054)

`test_integer_motion_v2_coverage.c`, `test_ssim_coverage.c` and
`test_integer_psnr_coverage.c` retain all nineteen registrations and
their order. Their private setup helpers preserve descriptor
lookup, option insertion, context creation/init, collector creation
and every assertion in order; callers immediately propagate first
failure. Keep existing ownership and teardown behavior, const
descriptor views and C NULL brackets. Helpers are not new
registered cases. See
[Research-2054](../../docs/research/2054-metric-coverage-const-2026-09-08.md).

## Large-fixture parity variants (ADR-1206)

Every CUDA and SYCL parity test is registered twice: once at its
own small fixture and once as `<name>_large` against 960x540, built
from same TU with `-DFIXTURE_W=960u -DFIXTURE_H=540u`. Fixture
macros are `#ifndef`-guarded for exactly this reason — do not
un-guard them.

960x540 is not arbitrary: `min(w, h) = 540` puts shared SSIM/MS-SSIM
auto-scale `max(1, round(min(w, h) / 256))` at 2, and 540 is not
multiple of 16/32-wide kernel blocks, so tail bounds are exercised
too. Below 384 px that auto-scale is always 1 and whole
resolution-dependent half of these extractors is unreachable —
which is where ADR-1202, ADR-1204 and `float_ssim` scale=1-only
limitation all hid.

When adding parity test, add it to matching
`*_parity_large_fixture_tests` list too. Two deliberate exceptions,
both documented in ADR-1206:

- `test_*_float_ssim_parity` stays registered but treats twin's
  `-EINVAL` at decimating resolution as **skip**, because GPU twins
  are v1 scale=1-only while CPU decimates — no parity to assert.
  Variant is kept so twin which stops refusing and starts returning
  scale=1 score fails loudly instead of silently comparing two
  metrics.
- `test_sycl_motion_add_uv_parity` is not registered at all: it
  compares float CPU against fixed-point SYCL, so its tolerance is
  per-fixture budget rather than bit-exactness bound.

HIP and Metal are not registered yet — unverifiable on current
workstation.

## Shared GPU test sources request only names every arm emits (T-HIP-ADM-TESTS-STALE-SHOULD-FAIL-2026-09-18)

`test_adm_small_border.c` and `test_adm_wide_rounding.c` build twice:
`-DHAVE_CUDA=1` and `-DHAVE_HIP=1`. Feature list must hold only names
the arm's twin provides. HIP `integer_adm` twin has no AIM pass, so
`VMAF_integer_feature_adm3_score` / `_aim_score` stay out of its
`provided_features[]` (T-GPU-ADM-AIM-DEVICE-PASS-MISSING-SYCL-HIP-2026-09-05);
asking for them returns `-EINVAL` from `vmaf_feature_score_at_index()`
before any parity check runs. Guard such names with
`#if !defined(HAVE_HIP)`. Print the failing name: a bare "failed" cost a
session to diagnose.

`should_fail : true` in `meson.build` needs a reason that is true today.
Meson counts an unexpected pass as a failure, so a stale marker breaks
`meson test` on every machine with the device. When the cited defect is
fixed, drop the marker in the same PR (ADR-1211 fixed the staging fault
the three HIP ADM markers cited; the markers outlived it by two weeks).

Parity fixtures must carry texture. Smooth ramps such as
`(row * 7 + col * 5) & 0xFF` leave the ADM contrast-masking kernel almost
nothing to accumulate: with the pre-ADR-1167 border defect planted back
into `adm_cm.hip`, the ramp moved adm2 by 7.5e-6, under the 1e-4 gate;
the lowbias32 texture in `luma_sample()` moves it by 4.0e-4. Before
trusting a new parity test, plant the defect it targets and watch it
fail. Rounding placement inside a row (per pixel, per warp, per row)
does not reach any emitted ADM score: the CPU divides the accumulator by
`2^(52 - shift_cub - shift_inner_accum)` and casts to `float`. No
score-level tolerance detects it
(T-ADM-CM-ROUNDING-PLACEMENT-UNOBSERVABLE-2026-09-19).
