<!-- markdownlint-disable MD060 -->
# ADR-0938: Feature-extractor coverage round 2 — seven CPU-side test executables

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: tests, coverage, ci

## Context

The 2026-05-31 Coverage Gate baseline showed seven CPU-side files
under `core/src/feature/` between 17 % and 80 % line coverage with no
in-flight PR claiming them:

- `integer_motion.c` (71.8 %)
- `integer_motion_v2.c` (17.6 %)
- `integer_psnr.c` (70.1 %)
- `integer_vif.h` (72.2 %)
- `iqa/convolve.c` (41.2 %)
- `barten_csf_tools.h` (45.5 %)
- `ms_ssim_decimate.c` (80.4 %)

PR #344 (round 1) owns `mkdirp.c`, `luminance_tools.c`, `feature_name.c`,
and `feature_extractor.c`; the file set above is disjoint.

The 90 % per-file floor (ADR-0114 trajectory) was unreachable without
exercising option-driven init paths (e.g. `min_sse`, `enable_apsnr`,
`motion_force_zero`, `motion_moving_average`, `motion_five_frame_window`),
HBD extract paths (10 / 12 / 16-bit), multi-frame `extract` + `flush`
flows, the `prev_ref`-driven `motion_v2` pipeline (which `libvmaf.c`
normally feeds at line 1543), the `integer_vif` log2 LUT inline helpers
(`log2_32` / `log2_64`), the `iqa` boundary-extension helpers
(`KBND_SYMMETRIC` / `KBND_REPLICATE` / `KBND_CONSTANT`), the
`barten_watson_blend_csf*` resolution dispatchers (4 × 2 resolution +
distance combinations plus the `-EINVAL` fallthrough on each variant),
and the `ms_ssim_decimate` runtime-dispatch wrapper with NULL
out-pointer handling.

## Decision

Add seven targeted test executables to `core/test/meson.build`, all in
the `fast` suite, mirroring the link-recipe of the existing
`test_speed_qa` / `test_psnr` / `test_iqa_convolve` targets:

- `test_integer_psnr_coverage`
- `test_integer_motion_coverage`
- `test_integer_motion_v2_coverage`
- `test_integer_vif_log2`
- `test_iqa_convolve_coverage`
- `test_barten_csf_coverage`
- `test_ms_ssim_decimate_coverage`

Each test file is pure test-only; no production code changes.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Extend existing files in-place (test_psnr.c, test_motion_min_dim.c, …) | Fewer meson edits | Mixes "rebase-sensitive upstream-mirror" tests (test_psnr.c) with fork-local coverage assertions; breaks the `git blame` history for the upstream files | Round 1 (PR #344) established the per-coverage-tranche `*_coverage.c` convention; round 2 keeps it. |
| Drive coverage via the existing `test_predict`/integration tests | Less duplication | Integration tests run a single canonical configuration; option-suffixed feature names and HBD bit depths are out of scope | Coverage of option-driven branches requires per-option tests. |
| Defer until the 90 % gate flips to required | Cheaper now | Round 1 already opened — round 2 keeps the trajectory; deferring lets coverage drift back as more code lands | Maintain forward pressure. |

## Consequences

- **Positive**: ~12-15 pp average jump per file in the targeted set;
  one file (`integer_vif.h` log2 helpers) reaches 100 %.
  `integer_motion_v2.c` jumps from 17.6 % → 88.2 % — the largest
  single-file uplift in round 2.
- **Negative**: 7 additional test binaries link against
  `libvmaf_feature_static_lib` + `libvmaf_cpu_static_lib`. Each adds
  ~50 ms to `meson test --suite=fast`. Total cost: ~350 ms wall.
- **Neutral / follow-ups**:
  - The static-inline `barten_csf_tools.h` only reaches 48.5 % because
    gcovr aggregates only one TU per static-inline header. The branch
    count rose (50 → 55) per the underlying `_adm_csf` consumer TU,
    confirming the test ran the new branches; the per-TU header
    accounting is a gcovr limitation, not a test gap.
  - `motion_v2`'s `prev_ref` is set by `libvmaf.c` (line 1543) in the
    framework path; unit tests now set it directly. Documented inline
    in the test file.

## References

- ADR-0114 (Coverage Gate baseline + 90 % per-file trajectory).
- ADR-0337 (`motion_v2` rejects `motion_five_frame_window=true` at
  init() with -ENOTSUP — exercised by round 2 test).
- PR #344 (round 1; sister scope, disjoint file set).
- Source: `req` — "Push test coverage in `core/src/feature/` toward
  the new 90% per-file floor. PR overlap to AVOID: PR #344."
