<!-- markdownlint-disable MD060 -->
# Feature-extractor coverage round 2 — research digest

- **Date**: 2026-05-31
- **ADR**: [ADR-0938](../adr/0938-feature-extractor-coverage-round2.md)
- **Status**: Completed
- **Owner**: lusoris

## Baseline

`meson setup build-cov core -Db_coverage=true -Denable_cuda=false
-Denable_sycl=false && ninja -C build-cov && meson test -C build-cov
--suite=fast --suite=simd --suite=dnn` against `origin/master` at
commit `45d536962f` (2026-05-31).

`gcovr -r .. --csv -e '.*test.*' -f '.*core/src/feature/.*'` baseline:

| File | Lines | Covered | Pct |
|---|---:|---:|---:|
| `integer_motion.c` | 213 | 153 | 71.8 % |
| `integer_motion_v2.c` | 119 | 21 | 17.6 % |
| `integer_psnr.c` | 87 | 61 | 70.1 % |
| `integer_vif.h` | 36 | 26 | 72.2 % |
| `iqa/convolve.c` | 102 | 42 | 41.2 % |
| `barten_csf_tools.h` | 66 | 30 | 45.5 % |
| `ms_ssim_decimate.c` | 46 | 37 | 80.4 % |

PR #344 (round 1) ships coverage for `mkdirp.c`, `luminance_tools.c`,
`feature_name.c`, `feature_extractor.c` — file set verified disjoint
via `gh pr view 344 --json files`.

## Targets selected

Files in the 17 %-80 % range with > 30 lines, no in-flight PR claim,
CPU-side (no GPU device dependency), low-cost test scaffolding
(matched to existing `test_speed_qa` / `test_psnr` /
`test_iqa_convolve` recipes).

Skipped: tiny-AI extractors (`feature_dists.c`, `feature_lpips.c`,
`feature_mobilesal.c`) — these have ~25 % baseline coverage but the
remaining code paths require a live ONNX session that the existing
`tiny_ai_test_template.h` cannot fake.

## Branch coverage strategy

| File | Branches plugged |
|---|---|
| `integer_psnr.c` | `pix_fmt=YUV400P` (line 124-125), `pix_fmt=YUV444P` + `min_sse>0` (line 128-136), HBD extract for 10 / 12 / 16-bit (line 263-266), `flush()` with `enable_apsnr=true` (line 277-292). |
| `integer_motion.c` | `motion_force_zero=true` (line 307-343), 4-frame default-window flow that lands the second-SAD branch (line 552-577), `motion_moving_average=true` (line 435-437), `motion_five_frame_window=true` + N<2 flush (line 441-455). |
| `integer_motion_v2.c` | init `-ENOTSUP` rejection of `motion_five_frame_window=true` (line 288-293), `index=0` short-circuit (line 356-360), `motion_force_zero=true` (line 350-354), 3-frame extract+flush with manually-set `prev_ref` (lines 161-204), `motion_moving_average=true` (line 475), 10-bit pipeline (lines 206-254). |
| `integer_vif.h` | `log2_32(2^16)` and `log2_32(2^15)` against closed-form expectation, `log2_64(2^17)` / `(2^32)` / `(2^40)`, monotonicity sweep across `[0x10000..0x80000)` for log2_32 and `[0x100000..0x1000000)` for log2_64. |
| `iqa/convolve.c` | `KBND_SYMMETRIC` in-range / negative-mirror / overflow-mirror, `KBND_REPLICATE` x/y under-/over-flow clamp, `KBND_CONSTANT` OOB returning the constant, `iqa_img_filter()` NULL kernel rejection, `iqa_img_filter()` owns-its-own-buffer path, `iqa_filter_pixel()` NULL kernel + edge / non-edge dispatch. |
| `barten_csf_tools.h` | All 8 supported (resolution, distance) pairs of `barten_watson_blend_csf` and `barten_watson_blend_csf_mae`, the `-EINVAL` fallthrough on both variants, mid-segment interpolation pairs `{0.005, 0.05, 0.5, 5.0, 50.0}` for `barten_csf` to drive every successive anchor-index selection. |
| `ms_ssim_decimate.c` | `ms_ssim_decimate_scalar(rw=NULL, rh=NULL)`, the runtime-dispatch wrapper `ms_ssim_decimate(...)` with explicit dispatch + NULL-out variants. |

## Final coverage

After `meson test -C build-cov --suite=fast --suite=simd --suite=dnn`
(68 tests, 100 % pass):

| File | Before | After | Delta |
|---|---:|---:|---:|
| `integer_motion.c` | 71.8 % | 81.7 % | +9.9 pp |
| `integer_motion_v2.c` | 17.6 % | 88.2 % | +70.6 pp |
| `integer_psnr.c` | 70.1 % | 92.0 % | +21.9 pp |
| `integer_vif.h` | 72.2 % | 100.0 % | +27.8 pp |
| `iqa/convolve.c` | 41.2 % | 98.0 % | +56.8 pp |
| `barten_csf_tools.h` | 45.5 % | 48.5 % | +3.0 pp* |
| `ms_ssim_decimate.c` | 80.4 % | 95.7 % | +15.3 pp |

\* `barten_csf_tools.h` is a static-inline header; gcovr aggregates a
single TU per header definition, so most of the new test exercises
land in the `test_barten_csf_coverage.c` TU which gcovr doesn't merge
back into the canonical `_adm_csf.c` consumer accounting. Branch
count rose 50→55, confirming the test ran the new paths.

## Smoke test command

```bash
git worktree add -b test/core-feature-coverage-round2 /tmp/wt-fea-r2 origin/master
cd /tmp/wt-fea-r2
meson setup build-cov core -Db_coverage=true -Denable_cuda=false -Denable_sycl=false
ninja -C build-cov
meson test -C build-cov --suite=fast --suite=simd --suite=dnn
gcovr -r .. --csv -e '.*test.*' -f '.*core/src/feature/.*' \
  --gcov-ignore-parse-errors=negative_hits.warn_once_per_file
```

Expected: 68 tests pass; the seven targeted files report at or
above the deltas in the table.

## Notable findings

- `motion_v2.extract` segfaults under `prev_ref=NULL` whenever the
  framework's `read_pictures` loop is bypassed (the unit test calls
  `vmaf_feature_extractor_context_extract` directly). Set `prev_ref`
  manually between frames; the test does this explicitly and clears
  it before `context_destroy` to avoid a double-free.
- Non-default `VMAF_OPT_FLAG_FEATURE_PARAM` options (e.g.
  `motion_force_zero=true`) cause `vmaf_feature_name_from_options` to
  suffix the feature name with `_<opt>=<val>`. Score-key assertions
  must use the dictionary-resolved name or be omitted (we omit them
  for coverage tests; the branch is exercised by the extract call,
  not by the score lookup).
- The 7 new test binaries together add ~350 ms of wall time to
  `meson test --suite=fast`.
