<!-- markdownlint-disable MD013 MD060 -->
# Research Digest 0732: Netflix Pipeline Functions Backlog Audit

**Status**: Complete
**Date**: 2026-05-28
**Scope**: Comprehensive audit of Netflix/vmaf upstream functions, features, models, and
pipeline steps not yet ported to the VMAFX fork (`VMAFx/vmafx`).
**Prior art**: `docs/research/feature-coverage-audit-2026-05-18.md` (ADR-0559),
`docs/research/netflix-upstream-feature-additions-since-sync-2026-05-18.md`
**ADR references**: ADR-0686 (VMAFX rebrand umbrella), ADR-0702 (Phase 4 foundation)

---

## Context and Methodology

The upstream remote `https://github.com/Netflix/vmaf.git` was fetched fresh on
2026-05-28. At that point `git log upstream/master ^HEAD --oneline` listed **2,251
upstream commits** not present in the fork. The fork carries the rebrand
(`libvmaf/` → `core/`, `python/vmaf/` → `compat/python-vmaf/`, org migration to
`VMAFx/vmafx`), so the topology is a port-only fork — no shared merge base with
upstream. Items were scored against the fork tree on the current `master` tip.

A prior audit (`feature-coverage-audit-2026-05-18.md`) covered CUDA-twin gaps,
model catalog, and feature extractor coverage as of 2026-05-18. This digest
extends that work, adds new upstream activity from May 2026, and synthesises a
prioritised backlog.

---

## Section 1: Inventory

### 1.1 C-side Feature Extractors

#### In upstream — fully ported to fork

All core feature extractors present in `libvmaf/src/feature/` on `upstream/master`
are present in `core/src/feature/` on the fork. The fork has gone further, adding:
`ssimulacra2`, `ciede`, `psnr_hvs`, `ms_ssim_decimate`, `speed_qa`, and the full
DNN-backed extractor family (`lpips`, `dists_sq`, `fastdvdnet_pre`, `mobilesal`,
`transnet_v2`).

#### In upstream — NOT yet fully reflected in fork

| Item | Upstream file / commit | Fork status | Notes |
|---|---|---|---|
| `integer_motion_v2` `motion_five_frame_window=true` mode | `a2b59b7750` | Registered option; returns error at init per ADR-0337 | The 5-frame window is rejected with a clear message; upstream's 5-frame mode is functional. Blocked on `prev_prev_ref` picture-pool plumbing that upstream resolved. |
| `integer_motion_v2` `motion_max_val` capping | `c17dd89806` | Present in fork (grep confirmed) | Already ported. |
| VIF horizontal boundary `reflect_101` fix (`float_vif.c`) | `bf9ad33340` | Not present in `float_vif.c` | Upstream fixed `compute_vif`'s horizontal boundary reflection to use `reflect_101`. Fork's `float_vif.c` pre-computes filters (ADR-0500 Win #3) but the scalar boundary-reflection fix has not been confirmed ported. Potential numeric drift for edge pixels at non-default `vif_kernelscale`. |
| CAMBI `calc_c_values_row_avx2` and `filter_mode_avx2` complete upstream shape | `767a6780e8`, `bd278ea6d2` | Fork cambi_avx2.c is 440 lines vs upstream 390; fork has these functions | Status: fork's `cambi_avx2.c` already contains `calculate_c_values_row_avx2` and `filter_mode_avx2`. PR #1545 was "about to land" per OPEN.md 2026-05-27 but is not visible in master log as of 2026-05-28. |
| `cambi.h` refactor: shared internal APIs moved from `cambi.c` | `41bacc83e1` | Fork's `cambi_internal.h` covers this | Verified: `cambi_internal.h` exists in fork. |
| `cambi_reciprocal_lut.h` | `d655cefe5d` | Fork has inline `reciprocal_lut` in `cambi.c` | Upstream introduced a standalone header; fork has the lut computed inline. Functionally equivalent; no separate `.h` file. |

#### In upstream — entirely new, not in fork

| Item | Upstream file / commit | Effort | Notes |
|---|---|---|---|
| `libvmaf` picture pool enabled by default (no `VMAF_PICTURE_POOL` guard) | `46d3a15456` | 2 | Fork still guards picture-pool and batch-threading with `#ifdef VMAF_PICTURE_POOL` / `#ifdef VMAF_BATCH_THREADING`. Upstream unconditionally enables both. |
| `libvmaf` batch threading enabled by default (no `VMAF_BATCH_THREADING` guard) | `dff4082b26` | 2 | Same — ifdef guard still present in fork's `core/src/libvmaf.c`. |
| `tools/vmaf` direct read enabled by default (`USE_DIRECT_READ`) | `e4b93c6edb` | 1 | Fork has `#ifdef USE_DIRECT_READ` compile-time guard in `core/tools/vmaf.c:328`. Upstream removed the guard. Low risk; fork can expose via `--netflix-compat` flag or just remove the guard. |

### 1.2 Python Harness (`compat/python-vmaf/`)

| Item | Upstream commit | Fork status |
|---|---|---|
| `SubjectiveDatasetReader` class (refactor of `read_dataset`) | `2e6bbb657d` | Not present. Fork `compat/python-vmaf/routine.py` still uses standalone `read_dataset()`. The upstream refactor adds per-video `workfile_yuv_type` overrides, separate ref/dis resample types, and `SubjectiveDatasetTester` class. |
| `VmafFeatureExtractor` v0.2.21 (aim, adm3, motion3 atom features) | `3dee966647` | Fork compat has VERSION `0.2.21` with `adm_skip_scale0` comment — version string matches but the atom features added upstream (`aim`, `adm3`, `motion3` in `ATOM_FEATURES`) are present. Status: already ported (confirmed `VERSION = "0.2.21"` in fork). |
| `SpeedChroma` / `SpeedTemporal` Python extractors | `7922f2c04c` | Not present in `compat/python-vmaf/`. C-side extractors exist (`core/src/feature/speed.c`), but the Python `SpeedChromaFeatureExtractor` / `SpeedTemporalFeatureExtractor` / quality runner classes are absent from the fork's compat layer. Upstream PR #1510. |
| `CambiFeatureExtractor` v0.8 | `095bb1818d` | Fork compat has VERSION `0.5`. Upstream v0.8 adds logic to avoid upscaling when encoding resolution is larger than input resolution. |
| CAMBI Python: mock-based `notyuv` tests, YUV fixtures replacing MP4 | `03e0dcb8c5`, `b2ec42c44d` | Not in fork tests. Minor test modernisation. |
| `executor.py` semaphore-based workfile sync | `662fb9cee1` | **Already in fork** — `compat/.../executor.py` has `multiprocessing.Semaphore` (confirmed). |
| `config.py` `download_reactively` race-safe atomic rename | `32780bd9b6` | **Already in fork** — `tempfile.mkstemp` + `os.replace` confirmed in fork's `config.py`. |
| Whitespace / import alignment sweep | `cf02b12671` | Cosmetic. Skip. |
| `MyTestCase` adoption wave (5 upstream test PRs) | `#1520–#1527` | Fork tests use their own `MyTestCase` mixin. The upstream `assertAlmostEqual` tolerances were updated for macOS FP precision. **Golden assertion values are different** — these upstream tolerance adjustments must not be applied to the fork without investigation (Rule 1). |
| BD-rate test data snake_case reformatting | `38e905d144` | Test-only cosmetic. |
| `VmafossexecCommandLineTest` stub removal | `25ff9f1886` | Clean-up only. |
| `fps` filter support in `Asset` / `call_vmafexec` | `560c4e4913` | Not present in fork compat. |
| `PyPsnrFeatureExtractor` rename (from `PypsnrFeatureExtractor`) | `b4c48fd30a` | Fork compat already uses `PyPsnrFeatureExtractor` (VERSION 0.2.21 confirms the rename is in). |

### 1.3 Models

The fork model directory is fully in sync with upstream (`model/`) for all released
Netflix SVM models (`vmaf_v0.6.1`, `vmaf_4k_v0.6.1`, `vmaf_b_v0.6.3`,
`vmaf_rb_v0.6.2/v0.6.3`, NEG variants, float variants). No new upstream model has
been released since the last sync. The fork adds 40+ files in `model/tiny/` and
`model/predictor_*.onnx` that are entirely fork-original.

**Outstanding**: `model/vmaf_hdr_model_card.md` is a placeholder. No HDR VMAF model
exists in upstream (confirmed 2026-05-18 by Research-0019; Netflix Issue #645 closed
without release). The fork's pre-positioned `speed_chroma`/`speed_temporal`
extractors await a future HDR model.

### 1.4 Pipeline Steps (Pre-processing / Quality Runner)

| Step | Upstream location | Fork status |
|---|---|---|
| `chroma_from_luma` correction in predictor | `core/src/predict.c` + JSON model field | Present. ADR-0574 (chroma correction parameter). |
| `vif_skip_scale0` option | `integer_vif.c` | Present in fork (`core/src/feature/integer_vif.c:77`). |
| `cambi_high_res_speedup` param documentation | `resource/doc` `721569bc1b` | Not yet reflected in fork docs under `docs/metrics/cambi.md`. |
| `motion_v2` docs update (motion2 score) | `721569bc1b` | Not yet reflected in fork docs under `docs/metrics/motion.md`. |
| `SubjectiveDatasetReader` per-video workfile overrides | `python/vmaf/routine.py` | Not ported (see §1.2). |

### 1.5 Research Papers vs Shipped Features

| Paper / Topic | Status |
|---|---|
| VMAF: The Journey Continues (2021) — NEG variants | Shipped: `vmaf_v0.6.1neg.json`, `vmaf_float_v0.6.1neg.json` |
| CAMBI (2022) | Shipped in both upstream and fork. Fork adds AVX2/AVX-512/NEON/GPU paths. |
| SpEED / SpEED-QA (2020, Bampis et al.) | C extractors present (`speed.c`). Python compat layer incomplete (§1.2). `speed_qa` NR scaffold present (ADR-0253). |
| SSIMULACRA2 | Present (fork-added; not in upstream Netflix/vmaf). |
| CIEDE2000 | Present in fork; not in upstream. |
| PSNR-HVS | Present in fork; not in upstream. |
| VMAF Neural / Deep-VMAF | No public release from Netflix. Not in upstream. No porting needed. |
| CHUG (Netflix HDR dataset) | Dataset acquired. Fork has HDR extraction scripts and CHUG feature pipeline. No upstream code equivalent. |

### 1.6 Upstream Performance and Build Improvements Not in Fork

| Item | Upstream commit | Fork status | Priority |
|---|---|---|---|
| Picture pool default-on | `46d3a15456` | Fork still guards behind `VMAF_PICTURE_POOL` ifdef | Medium |
| Batch threading default-on | `dff4082b26` | Fork still guards behind `VMAF_BATCH_THREADING` ifdef | Medium |
| Direct read default | `e4b93c6edb` | Fork has `USE_DIRECT_READ` guard | Low |
| `libvmaf/test`: 32-bit clang `-mfpmath=sse -msse2` toolchain | `9661232dcb` | Fork CI does not have a 32-bit clang matrix leg | Low |
| `vif.c` on-the-fly filter + `reflect_101` boundary fix | `bf9ad33340` | Fork uses pre-computed cache (ADR-0500), boundary fix needs verification | Medium |

---

## Section 2: Prioritised Backlog (Top 20)

Scoring: Value 1–5 (user-visible improvement), Effort 1–5 (1=hours, 5=weeks).
Ratio = Value / Effort (higher = port first).

| Rank | Item | Upstream ref | Value | Effort | Ratio | Action |
|---|---|---|---|---|---|---|
| 1 | `motion_five_frame_window=true` on `integer_motion_v2` — unblock 5-frame mode | `a2b59b7750` + picture-pool plumbing (ADR-0337) | 4 | 2 | 2.0 | Port — see ADR-0337 for unblock path |
| 2 | `SpeedChroma` / `SpeedTemporal` Python compat extractors | `7922f2c04c` | 4 | 2 | 2.0 | Port `SpeedChromaFeatureExtractor`, `SpeedChromaQualityRunner`, and temporal equivalents into `compat/python-vmaf/core/` |
| 3 | Picture pool and batch threading unconditional enable | `46d3a15456`, `dff4082b26` | 4 | 2 | 2.0 | Remove `#ifdef VMAF_PICTURE_POOL` and `#ifdef VMAF_BATCH_THREADING` guards from `core/src/libvmaf.c`; add regression test asserting both code paths execute |
| 4 | `CambiFeatureExtractor` Python compat to v0.8 | `095bb1818d` | 3 | 1 | 3.0 | Update VERSION `0.5` → `0.8`; port the "avoid upscaling when encode resolution > input" validation logic |
| 5 | VIF `reflect_101` horizontal boundary fix | `bf9ad33340` | 3 | 1 | 3.0 | Apply the boundary-reflection fix to `core/src/feature/float_vif.c`'s `compute_vif` scalar path; verify golden scores unaffected (the fix touches edge-pixel behaviour only) |
| 6 | `SubjectiveDatasetReader` class in `compat/python-vmaf/routine.py` | `2e6bbb657d` | 3 | 2 | 1.5 | Port the refactored class + `SubjectiveDatasetTester`; preserves `read_dataset()` wrapper. Enables per-video `workfile_yuv_type` overrides needed for mixed-bitdepth CHUG runs. |
| 7 | `cambi_high_res_speedup` and `motion2 score` doc updates | `721569bc1b` | 2 | 1 | 2.0 | Update `docs/metrics/cambi.md` and `docs/metrics/motion.md` to document the new param and corrected score semantics |
| 8 | `fps` filter support in `Asset` / `call_vmafexec` | `560c4e4913` | 2 | 1 | 2.0 | Port the fps filter parameter addition to `compat/python-vmaf/core/asset.py` |
| 9 | Direct read enabled by default in `vmaf` CLI | `e4b93c6edb` | 2 | 1 | 2.0 | Remove `#ifdef USE_DIRECT_READ` guard or expose via Meson option; update `--netflix-compat` flag docs |
| 10 | `chroma_correction_parameter` docs | implicit in model ecosystem | 2 | 1 | 2.0 | Add operator documentation for `chroma_from_luma` correction in `docs/metrics/adm.md` or a dedicated predictor-options page |
| 11 | `vif_skip_scale0` documentation | upstream tests reference it | 1 | 1 | 1.0 | Add operator docs entry in `docs/metrics/vif.md` |
| 12 | CAMBI Python `notyuv` test modernisation (mock + YUV fixtures) | `03e0dcb8c5` | 1 | 1 | 1.0 | Port the test cleanups for CI stability; minor |
| 13 | Upstream tolerance adjustments for macOS FP precision (MyTestCase wave) | `#1520–#1527` | 2 | 3 | 0.7 | Investigate whether the upstream `assertAlmostEqual` threshold changes apply to fork — do NOT apply blindly (Rule 1). Run the fork's Python golden tests on macOS CI and apply only deltas that pass the existing places=4 gate. |
| 14 | `VmafossexecCommandLineTest` stub removal | `25ff9f1886` | 1 | 1 | 1.0 | Delete the empty test class from fork compat test file if present |
| 15 | Whitespace / import alignment (`python/vmaf/` sweep) | `cf02b12671` | 1 | 1 | 1.0 | Apply with `make format` pass; cosmetic only |
| 16 | BD-rate test data snake_case reformatting | `38e905d144` | 1 | 1 | 1.0 | Cosmetic; port if BD-rate tests are run in fork CI |
| 17 | 32-bit clang CI matrix leg | `9661232dcb` | 2 | 3 | 0.7 | Fork does not target 32-bit; defer unless a reported regression |
| 18 | `cambi_reciprocal_lut.h` standalone header | `d655cefe5d` | 1 | 1 | 1.0 | Fork has the lut inline in `cambi.c`; the standalone header is a maintenance convenience but has no functional difference |
| 19 | Speed feature re-extraction for all corpora (CHUG, KoNViD-150k, Netflix, BVI-DVC) | Research-0559 gap | 4 | 4 | 1.0 | Run the extraction; blocked on compute availability and K150K refresh completion |
| 20 | CAMBI Python docs for `cambi_high_res_speedup` | `721569bc1b` | 2 | 1 | 2.0 | Bundled into rank-7 above |

---

## Section 3: Closure Recommendations

The following upstream items are recommended as **"will not port"** (deferred to
`docs/state.md` Deferred section) with rationale.

| Item | Reason |
|---|---|
| Upstream `python/test/` MyTestCase tolerance adjustments for macOS | The fork does not run macOS as a required CI gate for Python golden tests; upstream's tolerance relaxations would weaken the fork's `places=4` correctness contract. Port only if macOS Python CI is added with explicit investigation per Rule 1. |
| Upstream `python/test/` fixture slicing refactors (`322ca0412b`) | Test-internal. Fork uses its own fixture structure. |
| `VmafossexecCommandLineTest` removal | Verify the stub does not exist in fork before creating a PR; may already be absent. |
| Upstream cosmetic whitespace sweeps | Apply via `make format` in any touching PR; not worth standalone PRs. |
| Upstream CI retrigger commits | Not applicable to fork. |
| `python/test/` revert commits (`403dafed3e`, `eb3374d0f5`) | Not applicable; these are upstream experiment/revert cycles. |
| Deep-VMAF / VMAF Neural | No public implementation from Netflix. Would require fork-original research; track separately as a research item if pursued. |

---

## Section 4: Process Recommendations

### Re-run cadence

This audit should be re-run quarterly or immediately after a `git fetch upstream`
reveals more than 50 new non-merge commits to `upstream/master`. The command to
identify backlog delta:

```bash
git log upstream/master ^HEAD --oneline | grep -v "^[a-f0-9]* Merge"
```

### Ownership

- **C feature extractors**: assigned to the GPU/SIMD team; port via `/port-upstream-commit`.
- **Python compat layer**: assigned to the Python/AI team; port via standard PR.
- **Build system (ifdef removals)**: low-risk; any engineer can take these.
- **Models**: no new upstream models to port as of 2026-05-28.

### Triage point

New upstream commits should be triaged within one week of detection into:

1. **Port immediately** — bug fix with correctness impact (e.g., `reflect_101` fix,
   boundary clamp errors).
2. **Backlog** — new features or optimisations that the fork will adopt.
3. **Skip** — cosmetic, test-only changes with no user-visible delta.
4. **Investigate** — tolerance changes that might affect golden assertions.

This triage should be reflected in a `docs/state.md` row under "Open" or "Deferred"
within the same week.

### Upstream sync script

The `/sync-upstream` skill handles the git mechanics. The `port-upstream-commit`
skill handles single-commit cherry-picks with SIMD/GPU path adaptation. Both skills
should be the primary mechanism for porting; do not hand-edit the same TU as an
upstream commit without going through the skill (it handles meson/alias updates).

### Tracking state

Each item in the Section 2 backlog above maps to a backlog entry. As items close,
mark them in `docs/state.md` "Recently closed" with the PR number, commit SHA, and
ADR citation. The `.workingdir2/BACKLOG.md` "Upstream-port-later batch" section
should be updated to reflect the current actionable set after this audit lands.

---

## Appendix: Upstream Commits Examined

The following upstream commits (not in fork as of 2026-05-28) drove the backlog
items above. Commit SHAs are from `upstream/master`.

| SHA | Subject | Disposition |
|---|---|---|
| `e4b93c6edb` | tools/vmaf: enable direct read by default | Backlog #9 |
| `46d3a15456` | libvmaf: enable picture pool by default | Backlog #3 |
| `dff4082b26` | libvmaf: enable batch threading by default | Backlog #3 |
| `32780bd9b6` | python/config: make download_reactively race-safe | **Already in fork** |
| `4e46960105` | libvmaf/motion_v2: port remaining options | **Already in fork** |
| `a2b59b7750` | libvmaf/motion_v2: add motion_five_frame_window | Backlog #1 |
| `c17dd89806` | libvmaf/motion_v2: add motion_max_val | **Already in fork** |
| `856d383532` | libvmaf/motion_v2: fix mirroring behavior | **Already in fork** |
| `41bacc83e1` | feature/cambi: move shared code to cambi.h | **Already in fork** |
| `984f281f5b` | feature/cambi: fuse uh_slide/uh_slide_edge | **Already in fork** |
| `933cccb4bc` | feature/cambi: frame-level calc_c_values dispatch | **Already in fork** |
| `7747425138` | feature/cambi: compact histogram layout | **Already in fork** |
| `1091b0c190` | feature/cambi: add decimate_avx2 | **Already in fork** |
| `bd278ea6d2` | feature/cambi: add filter_mode_avx2 | **Already in fork** |
| `8c60dc9e22` | feature/cambi: skip histogram updates | **Already in fork** |
| `767a6780e8` | feature/cambi: refactor calculate_c_values_row, add avx2 | **Already in fork** |
| `9fad7317b7` | feature/cambi: factor 2D SAT recurrence | **Already in fork** |
| `d655cefe5d` | feature/cambi: add cambi_reciprocal_lut.h | **Already in fork** (inline) |
| `721569bc1b` | resource/doc: add cambi_high_res_speedup + motion2 | Backlog #7 |
| `662fb9cee1` | python: replace polling-based workfile sync with semaphores | **Already in fork** |
| `38e905d144` | python/test: BD-rate snake_case | Cosmetic / skip |
| `005988eadf` | python/test: MyTestCase fifo_mode / routine tests | Investigate (Rule 1) |
| `4679db83c2` | python/test: VMAFEXEC tolerance for macOS FP | Investigate (Rule 1) |
| `3e07510746` | python/test: MyTestCase vmafexec update | Investigate (Rule 1) |
| `e3827e4dd6` | python/test: MyTestCase asset/bootstrap/explainer | Investigate (Rule 1) |
| `25ff9f1886` | python/test: remove VmafossexecCommandLineTest stub | Backlog #14 |
| `3a041a9758` | python/test: MyTestCase test files | Investigate (Rule 1) |
| `cf02b12671` | python: align whitespace | Cosmetic |
| `ead2d12b03` | python/test: vif_scale3/adm3_egl_1 tolerances | Investigate (Rule 1) |
| `6c097fc4ef` | python/test: reduce ADM/VIF tolerances macOS | Investigate (Rule 1) |
| `7df50f3ae7` | python/test: align testutil fixtures | Minor |
| `322ca0412b` | python/test: replace temporal slicing | Fork test structure differs |
| `a333ba4c30` | python/test: remove tests requiring FFmpeg slicing | Not applicable |
| `74bdce1b5f` | python/test: align vmafexec_feature_extractor_test | Investigate (Rule 1) |
| `a377633524` | python/test: align feature_extractor_test | Investigate (Rule 1) |
| `3cbf352dab` | python/test: lts feature_extractor_test | Investigate (Rule 1) |
| `9fa593eb8e` | python/test: port aim/adm3/motion3 fextractor tests | **Already in fork** |
| `7d1ad54bda` | python/test: port feature extractor tests | **Already in fork** |
| `5c77700807` | python: extend VmafexecQualityRunner FEATURES | **Already in fork** |
| `3dee966647` | python: bump VmafFeatureExtractor to v0.2.21 | **Already in fork** (VERSION 0.2.21 confirmed) |
| `b4c48fd30a` | python: PypsnrMaxdb100 → PyPsnrMaxdb100 alias | **Already in fork** |
| `10ec73c730` | python/test: reduce speed_chroma_u_ks tolerance | Investigate (Rule 1) |
| `560c4e4913` | python: add fps filter support | Backlog #8 |
| `6a7b1ae348` | python/test: reduce speed_chroma_uv tolerance | Investigate (Rule 1) |
| `d4f6537210` | test: use pre-sliced 5-frame YUV for SpEED | N/A — fork has own fixtures |
| `7922f2c04c` | python: add SpeedChroma/SpeedTemporal extractors | Backlog #2 |
| `9661232dcb` | libvmaf/test: 32-bit clang toolchain | Skip |
| CAMBI Python v0.6–v0.8 (`095bb1818d`, `30a6e2a8dc`, `b2ec42c44d`, …) | cambi Python v0.8 + tests | Backlog #4, #12 |
| `2e6bbb657d` | python/routine: SubjectiveDatasetReader | Backlog #6 |
| `3685aa3c10` | python/test: SubjectiveDatasetReader tests | Companion to Backlog #6 |
| `bf9ad33340` | libvmaf/feature/vif: on-the-fly filter + reflect_101 fix | Backlog #5 (reflect_101 only — fork has better precompute) |
| `49d46e234b` | libvmaf/predict: port chroma_from_luma correction | **Already in fork** (predict.c confirmed) |
| `de53821641` | feature/integer_vif: port vif_skip_scale0 | **Already in fork** (integer_vif.c:77 confirmed) |
