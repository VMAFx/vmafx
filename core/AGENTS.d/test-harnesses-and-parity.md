---
paths:
  - core/test/meson.build
  - core/test/test_motion_avx512_parity.c
invariant: Fuzz harnesses track coverage; required aggregator rejects removed float_ansnr; AVX-512 verifies parity.
area: backends-metrics-and-tests
---
<!-- markdownlint-disable MD013 MD060 -->
# Fuzz harness coverage, required aggregator, and motion parity

- **Fuzz-harness coverage rule** (fork-local,
  [ADR-0270](../../docs/adr/0270-fuzzing-scaffold.md) +
  [ADR-0311](../../docs/adr/0311-libfuzzer-harness-expansion.md)): every
  attacker-reachable parser added under `core/tools/` must ship
  with matching libFuzzer harness under
  [`test/fuzz/`](../test/fuzz/) before merge — convention is one
  `fuzz_<surface>.c` + 3–6-seed corpus + row in
  `test/fuzz/meson.build` and
  [`.github/workflows/fuzz.yml`](../../.github/workflows/fuzz.yml)
  nightly matrix. Three harnesses currently ship: `fuzz_y4m_input`
  (Y4M parser), `fuzz_yuv_input` (raw-YUV reader), `fuzz_cli_parse`
  (CLI argv tokeniser + colon-delimited model/feature parsers).
  Harnesses re-include `tools/{y4m_input,yuv_input,vidinput,
  cli_parse}.c` as build inputs (via static-source path, not
  `libvmaf.so`); upstream sync splitting or renaming any of those
  source files needs corresponding `meson.build` source-list
  update *and* 60-second smoke run per harness against seed
  corpus. `__wrap_exit` longjmp shim in `fuzz_cli_parse.c` is
  GNU-ld / lld-specific, ships with `-Wl,--wrap=exit` link
  arg; document any platform expansion. Pre-commit hook
  enforcing new-parser-needs-new-harness contract is *not*
  yet wired — can be added later once at least 5 parsers carry
  harnesses.

- **Required-aggregator invariant — `float_ansnr` removal (PR #38 / ADR-0865):**
  `float_ansnr` was deliberately removed from all backends (CPU, CUDA, HIP, SYCL,
  Metal, Vulkan) in PR #38. Following must remain consistent on any rebase
  or upstream-sync touching these files:
  - `core/test/test_hip_smoke.c`: `test_float_ansnr_hip_extractor_registered`
    function and its `test_table[]` entry have been removed. Never restore them
    without also restoring HIP extractor source.
  - `compat/python-vmaf/core/feature_extractor.py` (line ~478):
    `VmafIntegerFeatureExtractor._generate_result()` must NOT list `float_ansnr`
    in its features. If upstream Netflix/vmaf adds `float_ansnr` back, re-add it
    in dedicated PR with CI verification.
  - `compat/python-vmaf/core/feature_extractor.py` (line ~463):
    `VmafIntegerFeatureExtractor.ATOM_FEATURES_TO_VMAFEXEC_KEY_DICT` must NOT
    map `"ansnr"` to `"float_ansnr"` while C library lacks extractor.
  Legacy path (`VmafFeatureExtractor`, line ~301) retains mapping as
  documented debt — tracked as T-LEGACY-RUNNER-ANSNR-BROKEN in `docs/state.md`.
  Checks run at configuration time (before `subdir()` calls) to catch misconfigurations early. Principle: every option depending on another must `error()` on bad combo, never silently no-op. See [`src/meson.build` lines 100–111, 74–76, 142–144](../src/meson.build).

## AVX-512 motion parity test invariant (ADR-0854)

- `core/test/test_motion_avx512_parity.c` provides direct bit-exact unit tests
  for all six AVX-512 motion kernels. If any of following functions is
  modified, corresponding test case **must** be re-run, must pass:
  - `motion_score_pipeline_8_avx512` (motion_v2_avx512.c)
  - `motion_score_pipeline_16_avx512` (motion_v2_avx512.c)
  - `sad_avx512` (motion_avx512.c)
  - `y_convolution_8_avx512` (motion_avx512.c)
  - `y_convolution_16_avx512` (motion_avx512.c)
  - `x_convolution_16_avx512` (motion_avx512.c)
- Scalar reference implementations in test file are line-for-line
  mirrors of production scalar paths. If scalar production path
  is changed (e.g. rounding bias, filter constants), update test's
  scalar reference accordingly, regenerate expected values.
- Test skips on hosts without `VMAF_X86_CPU_FLAG_AVX512`; this is
  intentional and correct. CI must run on AVX-512-capable host (see
  `.github/workflows/build.yml` x86_64 runner) for tests to be
  meaningful.
