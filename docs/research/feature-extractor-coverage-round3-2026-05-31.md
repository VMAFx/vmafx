<!-- markdownlint-disable MD013 MD060 -->
# Feature-extractor coverage round 3 — research digest

**Date**: 2026-05-31
**Baseline**: `master` @ `45d536962f` (CPU-only, fast+simd suites).
**Tool**: `gcovr 8.4` against `core/build-cov` (`-Db_coverage=true`).
**Scope**: `core/src/feature/` excluding `x86/`, `arm64/`, `cuda/`, `sycl/`, `hip/`, `dnn/`.
**Prior rounds**: PR #344 (round 1), PR #433 (round 2 — in flight).

## Baseline survey

Sorted by ascending line coverage (CPU-only build, fast+simd suites):

| File | Line % | Branch % | Status |
|---|---|---|---|
| `adm.c` | 0.0 | 0.0 | requires full extract pipeline |
| `adm_tools.c` | 0.0 | 0.0 | requires DWT setup |
| `feature_lpips.c` | 25.6 | 64 | ONNX-runtime gated |
| `feature_dists.c` | 25.6 | 64 | ONNX-runtime gated |
| `feature_mobilesal.c` | 25.3 | 54 | ONNX-runtime gated |
| `integer_adm.c` | 18.6 | n/a | requires full extract pipeline |
| `ciede.c` | 19.9 | n/a | requires full extract pipeline |
| `float_motion.c` | 17.1 | n/a | requires full motion pipeline |
| `transnet_v2.c` | 16.8 | n/a | ONNX-runtime gated |
| `fastdvdnet_pre.c` | 17.4 | 72 | ONNX-runtime gated |
| `float_ms_ssim.c` | 39.1 | 38 | min-dim test already exists; full coverage requires SSIM gauntlet |
| `integer_vif.c` | 40.6 | n/a | covered transitively by integer_vif tests |
| `iqa/convolve.c` | 41.2 | 25 | PR #433 → 95 % |
| `barten_csf_tools.h` | 45.5 | 45 | PR #433 → 100 % |
| `cambi.c` | 48.9 | 48 | helpers are static; needs full cambi_score |
| `integer_motion.h` | **58.3** | **40** | **picked — edge_16 mirror branches** |
| `integer_psnr.c` | 70.1 | 57 | PR #433 → 90 % |
| `integer_motion.c` | 71.8 | n/a | PR #433 → 82 % |
| `integer_vif.h` | 72.2 | n/a | PR #433 log2 → 100 % |
| `ms_ssim_decimate.c` | 80.4 | n/a | PR #433 → ~95 % |
| `feature_name.c` | 85.7 | 54 | PR #344 → 92 % |
| `speed_qa.c` | 86.6 | 56 | above range; HBD branch deferred |
| `feature_collector.cpp` | 90.1 | **62.5** | **picked — branch coverage push** |
| `integer_ssim.c` | 95.2 | n/a | already high |

## Decision matrix — what to push, what to skip

| File | Line / Branch | Verdict | Rationale |
|---|---|---|---|
| `integer_motion.h` | 58 / 40 | **PICK** | 12-line header; 5 mirror branches; clean unit test possible; not in PR #344/#433. |
| `adm_csf_tools.h` | 0 / 45 | **PICK** | 4-line inline; closed-form testable; not in PR #344/#433. |
| `feature_collector.cpp` | 90 / 62.5 | **PICK** | Just above 85 % line, but branch < 65 %; deterministic guards untested; not in PR #344/#433. |
| `cambi.c` | 49 / 48 | DEFER | Static helpers reachable only via cambi_score; risk of overlap with future cambi PRs. |
| `feature_dists.c` / `feature_lpips.c` / `feature_mobilesal.c` | 25 / 64 | DEFER | Extract paths need ONNX runtime + model; structural registration tests already exist. |
| `speed_qa.c` | 87 / 56 | DEFER | Above 85 % line band; HBD branch lives inside existing `test_speed_qa.c`. |
| `float_ms_ssim.c` | 39 / 38 | DEFER | Needs the full MS-SSIM gauntlet; better as a future "MS-SSIM coverage" PR. |
| `iqa/convolve.c` | 41 / 25 | SKIP — PR #433 | `test_iqa_convolve_coverage` already covers KBND_*, iqa_img_filter, iqa_filter_pixel. |
| `integer_vif.h` log2 | 72 | SKIP — PR #433 | `test_integer_vif_log2.c` already covers log2_32 / log2_64. |
| `integer_motion.c` / `integer_motion_v2.c` / `integer_psnr.c` / `barten_csf_tools.h` / `ms_ssim_decimate.c` | various | SKIP — PR #433 | Round 2 in flight on these files. |
| `feature_name.c` / `feature_extractor.c` / `luminance_tools.c` / `mkdirp.c` | various | SKIP — PR #344 | Round 1 in flight on these files. |

## Post-round measurements

After `meson test -C build-cov --suite=fast --suite=simd` (52/52 pass):

| File | Before | After | Delta |
|---|---|---|---|
| `integer_motion.h` | line 58.3 % / branch 40 % | line 100 % / branch 61.5 % | **+41.7 line / +21.5 branch** |
| `adm_csf_tools.h` | line 0 % / branch 45 % | line 75 % / branch n/a | **+75 line** (1 line still flagged due to gcov + `FORCE_INLINE` artifact on the declaration position) |
| `feature_collector.cpp` | line 90.1 % / branch 62.5 % | line 91.5 % / branch 71.9 % | **+1.4 line / +9.4 branch** |

## Reproducer

```bash
git fetch origin
git checkout -b test/core-feature-coverage-round3 origin/master
meson setup build-cov core -Db_coverage=true -Denable_cuda=false -Denable_sycl=false
ninja -C build-cov
meson test -C build-cov --suite=fast --suite=simd
gcovr --root . --filter 'core/src/feature/' \
      --exclude 'core/src/feature/x86/' --exclude 'core/src/feature/arm64/' \
      --exclude 'core/src/feature/cuda/' --exclude 'core/src/feature/sycl/' \
      --exclude 'core/src/feature/hip/' --exclude 'core/src/feature/dnn/' \
      --gcov-ignore-parse-errors=negative_hits.warn_once_per_file --txt
```

The three new test binaries (`test_integer_motion_edge16_coverage`,
`test_adm_csf_tools_coverage`, `test_feature_collector_coverage`)
each run in ≤10 ms; the total fast suite is unchanged in wall time.

## Limitations

- `adm_csf_tools.h` shows line 50 (the first body statement) as
  uncovered despite the function body executing. This is a known
  gcov interaction with `FORCE_INLINE`'d declarations placed across
  the declaration / first-statement boundary. The function value
  itself is verified against a closed-form reference, so the gap is
  an instrumentation artefact, not a real coverage hole.
- `feature_collector.cpp` branch coverage stalls at 71.9 % because
  the remaining gaps are realloc-OOM paths (lines 245, 251, 255,
  258, 261, 265-271) that need an allocator stub. Out of scope for
  this round.

## Cross-references

- ADR-0948 — this round's decision record.
- ADR-0114 — Coverage Gate (37 % global floor).
- PR #344 — Coverage round 1.
- PR #433 — Coverage round 2 (in flight, see
  `docs/research/feature-extractor-coverage-round2-2026-05-31.md`).
