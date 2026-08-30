<!-- markdownlint-disable MD060 -->
# Coverage Gap Analysis — 2026-05-02 Baseline

**Date:** 2026-05-02
**Branch / Commit:** `master` @ `bb9d772ec620d197a9166a092b8afce3517ea78d`
  *(latest successful CI run; current `master` tip `229ca74e` is queued at
  the time of writing — the artifact below represents the most recent
  attested baseline.)*
**Source:** GitHub Actions run `25263407343` (workflow
`tests-and-quality-gates.yml`, job `Coverage Gate`), artifact
`coverage-cpu` (gcovr 8.x output: `coverage.txt` / `coverage.json` /
`coverage.xml`).
**Tool:** `gcovr` (per ADR-0110: deduplicated `.gcno` aggregation,
`-fprofile-update=atomic`, `--num-processes 1`).
**Build flags used by the gate:**
`meson setup build-coverage --buildtype=debug -Db_coverage=true`
`-Denable_cuda=false -Denable_sycl=false -Denable_float=true`
`-Denable_avx512=true -Denable_dnn=enabled`
`-Dc_args=-fprofile-update=atomic -Dcpp_args=-fprofile-update=atomic`.

> **Status of this document:** investigation report, *no source code
> changes*. The recommendations become T-row backlog candidates.

## 1. Headline numbers

| Metric                         | Value           | Gate target      | Delta            |
|--------------------------------|-----------------|------------------|------------------|
| **Overall line coverage**      | **43.2 %**      | 70 % (final)     | −26.8 pp         |
| Current floor (CI gate)        | 40 %            | (ratchet)        | +3.2 pp headroom |
| Branch coverage                | 33.9 %          | (no gate yet)    | —                |
| Function coverage              | 56.8 %          | (no gate yet)    | —                |
| Files instrumented             | 126             | —                | —                |
| Files **below 70 %**           | **60 / 126**    | —                | —                |
| Critical-path files (dnn/, opt.c, read_json_model.c) | all ≥ 78 % where tested (per-file overrides ADR-0114) | 85 % | meets gate |

Coverage gate currently passes: floor is 40 %, achieved is 43.2 %.
The 70 % final target is the long-term ratchet; we are 26.8 pp short.

Synthetic projections (CI floor calc only — does not change the gate):

| View                                | Lines covered / total | %       |
|-------------------------------------|-----------------------|---------|
| All instrumented code               | 11 552 / 26 768       | 43.2 %  |
| Excluding GPU runtime + GPU kernels | 11 552 / 26 707       | 43.3 %  |
| Excl. GPU + uncovered SIMD files    | 11 552 / 21 712       | 53.2 %  |
| Excl. GPU + **all** SIMD files      |  7 019 / 16 157       | 43.4 %  |

Reading: dropping the 15 zero-covered SIMD `.c` files lifts the headline
to ~53 %. Dropping every SIMD file (incl. covered AVX2/AVX-512 paths)
moves it back to ~43 % — i.e. the SIMD paths that *are* exercised
contribute meaningfully and shouldn't be excluded; only the dead AVX2
files (no AVX2-only test case) are pure noise.

## 2. Coverage by bucket

| Bucket            | Files | Lines  | Covered | Pct    |
|-------------------|-------|--------|---------|--------|
| `feature_cpu`     |   63  |  9 564 |  3 652  | 38.2 % |
| `simd_x86`        |   33  | 10 550 |  4 533  | 43.0 % |
| `core` (libvmaf, predict, output, pdjson, …) | 21 | 4 598 | 1 892 | 41.1 % |
| `tools` (CLI)     |    1  |    616 |    341  | 55.4 % |
| **`dnn`**         |    8  |  1 440 |  1 134  | **78.8 %** |
| GPU runtime       |    0  |      0 |      0  | n/a (filtered out — no `.gcda`) |

The `dnn` bucket is the only one already on-target for the 70 % overall
ratchet; all other CPU buckets need work. GPU code (`/cuda/`, `/sycl/`,
`/vulkan/`) does not appear in the report at all because the gate's
build runs `-Denable_cuda=false -Denable_sycl=false`. GPU coverage is
tracked separately by the `coverage-gpu` job (self-hosted runner,
non-required) — out of scope here.

## 3. Top under-covered files (excluding GPU + SIMD)

Filtered to ≥ 50 lines so trivially-small files don't dominate.

| Pct    | Lines | File                                                    | Bucket          |
|--------|-------|---------------------------------------------------------|-----------------|
|  0.0 % |   103 | `core/src/feature/common/blur_array.c`               | feature_cpu     |
|  0.0 % |    57 | `core/src/feature/float_moment.c`                    | feature_cpu     |
|  0.0 % |   115 | `core/src/feature/integer_motion_v2.c`               | feature_cpu     |
|  0.0 % |    73 | `core/src/feature/motion.c`                          | feature_cpu     |
|  0.0 % |   712 | `core/src/feature/speed.c`                           | feature_cpu     |
|  0.0 % |   480 | `core/src/feature/ssimulacra2.c`                     | feature_cpu     |
|  0.0 % |   161 | `core/src/feature/third_party/xiph/psnr_hvs.c`       | feature_cpu     |
|  0.0 % |    61 | `core/src/gpu_picture_pool.c`                        | core (GPU glue) |
|  8.5 % | 1 959 | `core/src/svm.cpp`                                   | core            |
| 15.3 % |   131 | `core/src/feature/transnet_v2.c`                     | feature_cpu     |
| 16.3 % |   123 | `core/src/feature/fastdvdnet_pre.c`                  | feature_cpu     |
| 18.4 % | 1 619 | `core/src/feature/integer_adm.c`                     | feature_cpu     |
| 21.3 % |   254 | `core/src/feature/ciede.c`                           | feature_cpu     |
| 22.7 % |    88 | `core/src/feature/feature_mobilesal.c`               | feature_cpu     |
| 23.3 % |    90 | `core/src/feature/feature_lpips.c`                   | feature_cpu     |
| 28.3 % |   230 | `core/src/output.c`                                  | core            |
| 29.5 % |   352 | `core/src/feature/vif_tools.c`                       | feature_cpu     |
| 33.5 % |   669 | `core/src/feature/adm_tools.c`                       | feature_cpu     |
| 36.5 % |    85 | `core/src/feature/ansnr_tools.c`                     | feature_cpu     |
| 45.4 % |   302 | `core/src/predict.c`                                 | core            |
| 52.2 % |   556 | `core/src/pdjson.c`                                  | core            |
| 55.4 % |   616 | `core/src/libvmaf.c`                                 | core            |
| 62.6 % |   163 | `core/src/model.c`                                   | core            |

The **top three** non-GPU/non-SIMD targets by absolute uncovered line
count are:

1. `core/src/svm.cpp` — 1 959 lines, 8.5 %
   (~1 793 uncovered lines). Upstream-imported libsvm; only the
   prediction path is exercised by classic VMAF model inference. The
   training path (`svm_train`, `svm_save_model`, `svm_check_parameter`,
   the SMO solver) is dead in production but still instrumented.
2. `core/src/feature/integer_adm.c` — 1 619 lines, 18.4 %
   (~1 321 uncovered lines). Fixed-point ADM scale 1–4 inner loops, dwt
   stages, plus the 8/10/12-bit width specialisations.
3. `core/src/feature/adm_tools.c` — 669 lines, 33.5 %
   (~445 uncovered lines). Float ADM helper paths (CSF tables, decouple,
   contrast-mask), exercised partially by the float ADM test.

## 4. Critical-path status

`scripts/ci/coverage-check.sh` flags files under `core/src/dnn/`,
`opt.c`, `read_json_model.c` as security-critical (85 % min, with
ADR-0114 per-file overrides for `ort_backend.c` / `dnn_api.c` at 78 %
and `tiny_extractor_template.h` at 10 %).

| File                                            | Pct     | Threshold | Status |
|-------------------------------------------------|---------|-----------|--------|
| `dnn/dnn_api.c`                                 | 78.0 %  | 78 %      | PASS (ADR-0114) |
| `dnn/dnn_attach_api.c`                          |  0.0 %  | not enforced (zero-tested) | gap |
| `dnn/model_loader.c`                            | 86.0 %  | 85 %      | PASS |
| `dnn/onnx_scan.c`                               | 93.4 %  | 85 %      | PASS |
| `dnn/op_allowlist.c`                            | 100.0 % | 85 %      | PASS |
| `dnn/ort_backend.c`                             | 79.3 %  | 78 %      | PASS (ADR-0114) |
| `dnn/tensor_io.c`                               | 89.9 %  | 85 %      | PASS |
| `dnn/tiny_extractor_template.h`                 | 10.4 %  | 10 %      | PASS (ADR override) |
| `opt.c`                                         | 100.0 % | 85 %      | PASS |
| `read_json_model.c`                             | 88.9 %  | 85 %      | PASS |

Gate-critical situation: clean. The single sub-threshold critical file
is `dnn_attach_api.c` (39 instrumented lines, **0 % covered**) — it's
classified "no tests yet, not enforced" by the gate. Worth noting that
this is the EP-attach helper added in the CUDA EP work; once an
EP-equipped test exists it should clear the 85 % bar trivially.

## 5. Recommendations (3–5 concrete next-step targets)

Each recommendation lists projected impact on overall line coverage and
a rough scope estimate. Numbers are approximate (assumes ~80 % branch
coverage on touched lines).

### R1 — Exclude libsvm training path from the coverage corpus (Priority: HIGH)

- **What:** Add `core/src/svm.cpp` to the `gcovr --exclude` filter
  in `tests-and-quality-gates.yml`, or split it so only the prediction
  half (`svm_predict_*`, `svm_load_model`, `svm_destroy_*`) is
  instrumented. The training half is unreachable from libvmaf at runtime
  (we never call `svm_train`).
- **Impact:** Removes ~1 793 dead uncovered lines from the denominator.
  Headline jumps from **43.2 % → ~50.7 %** with zero new test code.
- **Scope:** ~5 lines of YAML, optionally a small `core/meson.build`
  hunk plus one ADR documenting the decision (analogue to the
  `tiny_extractor_template.h` carve-out). **~30 min, no test fixtures.**
- **Risk:** Must document the reasoning (otherwise future maintainers
  re-add coverage and trip the floor). Cite under-test parity:
  Netflix-upstream uses the same prediction-only surface.

### R2 — Add an `integer_adm` golden-equivalence test (Priority: HIGH)

- **What:** Extend `python/test/feature_extractor_test.py` (or a new C
  unit test under `core/test/`) with a 1080p YUV fixture that walks
  `integer_adm.c` scale 0–3 across 8/10/12-bit widths. The Netflix
  golden fixtures already cover one width — we need the other two
  bit-depths to hit the dead specialisations (most of the 1 321
  uncovered lines are `_8bit` / `_10bit` / `_12bit` template
  expansions).
- **Impact:** Realistically lifts `integer_adm.c` from 18 % to ~60 %
  (~700 newly covered lines). Headline: **+2.6 pp** (toward 46 %).
- **Scope:** Reuse existing 10-bit checkerboard YUVs + add one
  12-bit fixture; ~80 lines of test code. **~3 h, including snapshot
  baseline regen.**
- **Caveat:** Need to confirm the 12-bit decode path on the CPU
  reference works headlessly — if not, scope grows to ~5 h.

### R3 — Add `output.c` writer-format unit tests (Priority: MEDIUM)

- **What:** `output.c` ships JSON / XML / CSV / SUB writers (`vmaf_write_output_*`).
  The `--precision` work touched all four but only JSON has end-to-end
  coverage. Add a meson unit test that round-trips a small synthetic
  `VmafFeatureCollector` through each writer, asserting the output is
  parseable and contains expected score fields.
- **Impact:** `output.c` 28 % → ~85 %. Headline: **+0.5 pp**. Catches
  regression-prone branches (subsample handling, NaN guards,
  per-frame vs pooled paths).
- **Scope:** ~150 lines of C test code, no new fixtures (synthetic
  collector). **~2 h.**

### R4 — Add `predict.c` model-prediction smoke test (Priority: MEDIUM)

- **What:** `predict.c` is at 45 %. It's the bridge between
  `feature_collector` and `svm.cpp` / per-feature pooling.  Add a unit
  test that loads a small `.json` VMAF model, feeds a hand-built
  feature vector, and asserts the predicted score within a tolerance.
- **Impact:** `predict.c` 45 % → ~75 %. Headline: **+0.3 pp**.
  Closes the only currently-untested entry point for transformed-feature
  fallbacks (`init_predict_score_transform_apply`).
- **Scope:** ~80 lines of test code + reuse an existing model JSON.
  **~1.5 h.**

### R5 — Carve out demonstrably-dead extractors (Priority: LOW, defer)

- **What:** `feature/transnet_v2.c`, `feature/feature_mobilesal.c`,
  `feature/feature_lpips.c`, `feature/fastdvdnet_pre.c` are placeholder
  research extractors at 15–23 % coverage. They're not user-facing yet
  (no model JSON registered against them on master). Either (a) wire
  them into a smoke fixture or (b) move them to `enabled_dnn=false`
  conditional compilation so they drop out of the coverage corpus
  unless someone actively iterates on them.
- **Impact:** Either path lifts the headline by **~1.0 pp** (drops
  ~330 uncovered lines from the denominator) or covers them properly.
- **Scope:** ~1 h to carve out + ADR; ~6 h to add real extractor smoke
  tests.
- **Why low priority:** these are part of the tiny-AI roadmap; rather
  than gate them now, defer to whoever lands the next tiny-AI extractor
  and have them pull this in alongside.

### Combined projection

Doing **R1 + R2 + R3 + R4** together moves the headline from
**43.2 % → ~50 %** (R1 alone) → **~53 %** (R1+R2) → **~54 %**
(R1+R2+R3+R4). That's roughly half the gap to the 70 % final target,
without touching SIMD or GPU code. The remaining gap is concentrated
in `cambi.c` (54 %), `feature_collector.c` (78 %),
`feature_extractor.c` (66 %), `pdjson.c` (52 %), `libvmaf.c` (55 %) —
all of which need targeted unit tests rather than refactors.

## 6. SIMD / GPU exclusion notes

- **x86 SIMD (33 files, 10 550 lines, 43 % covered overall):** the
  CI runner is AVX-512-capable (`enable_avx512=true`), so AVX-512
  paths *are* exercised — `convolve_avx512` 92 %, `motion_avx512` 93 %,
  `vif_avx512` ~90 %. The 15 zero-covered files are mostly AVX2-only
  variants where the dispatcher prefers AVX-512 on the CI runner
  (`adm_avx2.c`, `motion_avx2.c`, `psnr_avx2.c`, `ssim_avx2.c`,
  `cambi_avx2.c`, `cambi_avx512.c` — last one zero because no Cambi
  AVX-512 test fixture). Not a real gap on master CI; would need an
  AVX-2-only matrix entry to exercise.
- **arm64 SIMD:** not built on the gate (Linux x86 runner). Tracked
  by the cross-arch matrix job, not the coverage gate.
- **GPU (CUDA, SYCL, Vulkan, HIP):** filtered out by
  `-Denable_cuda=false -Denable_sycl=false`. Coverage tracked
  separately by the `coverage-gpu` self-hosted job; not enforced as a
  required check yet.

## 7. Toolchain / parity notes

The local reproduction command (per the workflow):

```bash
cd libvmaf
meson setup build-coverage --buildtype=debug \
  -Db_coverage=true -Denable_cuda=false -Denable_sycl=false \
  -Denable_float=true -Denable_avx512=true \
  -Denable_dnn=enabled \
  -Dc_args=-fprofile-update=atomic -Dcpp_args=-fprofile-update=atomic
ninja -C build-coverage
meson test -C build-coverage --print-errorlogs --num-processes 1
# Python suite step (requires PYTHONPATH + LD_LIBRARY_PATH set up; see workflow)
gcovr --root .. --filter 'src/.*' --exclude '.*/test/.*' \
  --exclude '.*/tests/.*' --exclude '.*/subprojects/.*' \
  --gcov-ignore-parse-errors=negative_hits.warn \
  --gcov-ignore-parse-errors=suspicious_hits.warn \
  --print-summary --txt build-coverage/coverage.txt \
  --json-summary build-coverage/coverage.json \
  build-coverage
scripts/ci/coverage-check.sh core/build-coverage/coverage.json 40 85
```

This investigation used the CI artifact directly
(`gh run download 25263407343 -n coverage-cpu`) rather than a local
build, both for runtime parity and to avoid a 25-minute local
instrumented build.

## 8. References

- ADR-0110 — gcovr migration / atomic profile updates / serial test runs.
- ADR-0114 — per-file critical thresholds for `dnn_api.c` /
  `ort_backend.c` (EP-availability ceiling).
- ADR-0117 — gcovr `--gcov-ignore-parse-errors` filtering.
- `docs/principles.md` §3 — 70 % overall / 85 % critical target.
- Workflow: `.github/workflows/tests-and-quality-gates.yml` (lines
  633–793).
- Gate script: `scripts/ci/coverage-check.sh`.
