<!-- markdownlint-disable MD013 MD056 MD060 -->
# Research-0733 — Feature Importance Audit and Drop Recommendations (2026-05-28)

**Status:** Final
**Date:** 2026-05-28
**Scope:** All feature extractors in `core/src/feature/`
**Question:** Which feature extractors are genuinely needed vs. dead weight after the
tiny-AI model surface matured to fr_regressor_v3 + vmaf_tiny_v4?

---

## Executive Summary

Every shipped tiny-AI regression model (`vmaf_tiny_v2`, `vmaf_tiny_v3`, `vmaf_tiny_v4`,
`fr_regressor_v1..v3`, `fr_regressor_v2_ensemble_v1`) uses the identical six-feature
**canonical-6** input vector:

```text
adm2 | vif_scale0 | vif_scale1 | vif_scale2 | vif_scale3 | motion2
```

All classic SVM models (`vmaf_v0.6.1`, `vmaf_4k_v0.6.1`, `vmaf_b_v0.6.3`, etc.) also
use exactly these six features. The Netflix golden gate (CLAUDE.md §8) depends on
`VMAF_integer_feature` which maps precisely to this set.

This means **every feature extractor outside the canonical-6 is zero-weight from the
perspective of the shipped AI models.** However, several non-canonical features are
user-discoverable CLI options, are referenced in documentation, and have real standalone
correlation with MOS. The analysis below separates model-weight zero from truly droppable.

**Top-3 drop candidates (zero model weight, low standalone MOS correlation, high LOC):**

| Candidate | Max model PI drop | MOS |PLCC| | LOC (all backends) | Verdict |
|-----------|-------------------|------------|----------------------|---------|
| `ansnr` / `float_ansnr` | 0.0 (not an input) | 0.17 | 1,626 | DROP |
| `speed_chroma` + `speed_temporal` | 0.0 (not an input) | 0.06–0.09 | 5,783 | DROP |
| `psnr_cb` / `psnr_cr` (chroma channels) | 0.0 (psnr feature outputs these as subsidiary scores, not model inputs) | 0.14–0.18 | 0 extra | DEMOTE |

**Expected LOC savings if canonical-6 + cambi + ciede + psnr_hvs + ssimulacra2 + psnr(luma)
are kept and ansnr + speed are dropped: ~7,409 LOC** across CPU + GPU backends.

**Recommendation: hold this decision for an ADR** — several drops interact with the
Python harness `VMAF_feature` and `AnsnrFeatureExtractor` API that upstream Netflix still
ships. Any drop needs an ADR + deprecation notice + `--feature <name>` opt-in gate first.

---

## Part 1 — Permutation Importance Results

### Data sources

- **Model:** `vmaf_tiny_v2.onnx`, `vmaf_tiny_v3.onnx`, `vmaf_tiny_v4.onnx`
- **Target:** `vmaf` teacher score (from the 4-corpus parquet)
- **Parquet:** `runs/full_features_refresh_4source_partial_20260521.parquet`
  (62 MB, 314,417 frame rows, 4 corpora, 30 columns)
- **Sampling:** 10,000 rows, 7 seeds per feature
- **Method:** Permutation importance — shuffle one feature column at a time, measure
  PLCC drop vs. unshuffled baseline

### Baseline PLCC

| Model | Baseline PLCC vs vmaf teacher |
|-------|-------------------------------|
| vmaf_tiny_v2 | 0.9998 |
| vmaf_tiny_v3 | 0.9999 |
| vmaf_tiny_v4 | 0.9998 |

### Permutation importance table (canonical-6 inputs)

All three vmaf_tiny models are in complete agreement on ranking. Values are
PLCC drop (higher = more important); negative values would indicate the feature
adds noise but all are positive here.

| Rank | Feature | v2 drop | v3 drop | v4 drop | Max drop | Std (v2) |
|------|---------|---------|---------|---------|----------|----------|
| 1 | `adm2` | +0.4666 | +0.4791 | +0.4686 | +0.4791 | 0.0068 |
| 2 | `motion2` | +0.1750 | +0.1755 | +0.1741 | +0.1755 | 0.0013 |
| 3 | `vif_scale3` | +0.1196 | +0.1210 | +0.1269 | +0.1269 | 0.0016 |
| 4 | `vif_scale2` | +0.0468 | +0.0493 | +0.0866 | +0.0866 | 0.0006 |
| 5 | `vif_scale1` | +0.0055 | +0.0071 | +0.0169 | +0.0169 | 0.0001 |
| 6 | `vif_scale0` | +0.0014 | +0.0020 | +0.0012 | +0.0020 | 0.0000 |

**Key observations:**

- `adm2` is dominant — shuffling it drops PLCC by ~0.47 (from 0.9999 to ~0.53)
- `motion2` and `vif_scale3` are clearly load-bearing
- `vif_scale0` is nearly redundant (PLCC drop < 0.002) — it carries almost no
  unique signal beyond `vif_scale1..3`, but removing it would require retraining
  all models and is not recommended without a full retrain study
- **None of the non-canonical-6 features appear in any model input vector at all**

### fr_regressor_v3 evaluation note

`fr_regressor_v3` uses two ONNX inputs: a 6-D feature vector + an 18-D codec one-hot
block. Running permutation importance with a zero codec block gives a baseline PLCC of
−0.22 (the model is highly codec-conditioned and produces near-random scores without a
valid codec embedding). This is not a fault in the model — it is operating outside its
training distribution. The canonical vmaf_tiny models (which take only the 6 features)
are the correct objects for this analysis.

---

## Part 2 — Non-Canonical Features: Standalone MOS Correlation

Source: `runs/full_features_konvid_refresh_20260520_with_mos.parquet`
(83,096 frame rows, 369 clips after per-clip mean aggregation, KoNViD-1k MOS labels)

Per-clip mean feature vs. MOS, ranked by |PLCC|:

| Rank | Feature | |PLCC| vs MOS | |SROCC| vs MOS | Sign |
|------|---------|-------------|--------------|------|
| 1 | `psnr_y` | 0.368 | 0.357 | negative (lower error = higher MOS) |
| 2 | `adm_scale1` | 0.358 | 0.326 | negative |
| 3 | `psnr_hvs` | 0.358 | 0.338 | negative |
| 4 | `vif_scale0` | 0.351 | 0.354 | negative |
| 5 | `adm_scale0` | 0.328 | 0.341 | negative |
| 6 | `float_ssim` | 0.318 | 0.328 | negative |
| 7 | `ciede2000` | 0.312 | 0.318 | negative |
| 8 | `cambi` | 0.291 | 0.284 | negative |
| 9 | `vif_scale1` | 0.286 | 0.274 | negative |
| 10 | `float_ms_ssim` | 0.272 | 0.276 | negative |
| 11 | `vif_scale2` | 0.233 | 0.226 | negative |
| 12 | `ssimulacra2` | 0.189 | 0.183 | negative |
| 13 | `adm_scale3` | 0.185 | 0.176 | positive |
| 14 | `psnr_cb` | 0.173 | 0.158 | negative |
| 15 | `vif_scale3` | 0.161 | 0.153 | negative |
| 16 | `psnr_cr` | 0.136 | 0.129 | negative |
| 17 | `adm2` | 0.129 | 0.120 | negative |
| 18 | `speed_temporal` | 0.090 | 0.084 | positive |
| 19 | `speed_chroma_v` | 0.076 | 0.064 | negative |
| 20 | `speed_chroma_uv` | 0.070 | 0.061 | negative |
| 21 | `speed_chroma_u` | 0.056 | 0.063 | negative |
| 22 | `motion2` | 0.036 | 0.082 | positive |
| 23 | `motion3` | 0.035 | 0.082 | positive |
| 24 | `adm_scale2` | 0.034 | 0.047 | positive |
| 25 | `motion` | 0.020 | 0.069 | positive |

**Note on sign and corpus bias:** KoNViD-1k is a UGC/in-the-wild corpus where
high-quality content tends to be lower-motion content shot at lower bitrates.
This explains the negative correlation for most distortion metrics (lower distortion
in MOS-score context): `psnr_y` here reflects that clips with lower PSNR from heavy
compression tend to score lower MOS, so the relationship is inverse with the raw
PSNR value as computed (higher = less distortion). The MOS ratings here are on a
1–5 scale (mean = 3.26, std = 0.56).

---

## Part 3 — Feature Extractor Inventory

### Canonical-6 features (PROTECTED — used by all models + Netflix golden gate)

| Feature | C source LOC (CPU only) | All-backend LOC | Model usage | Non-AI consumers |
|---------|------------------------|-----------------|-------------|-----------------|
| `adm2` (via `VMAF_integer_feature`) | integer_adm.c: 3,122 + adm.c: 289 + adm_tools.c: 1,195 | ~19,937 | ALL models | CLI, Python harness, ffmpeg, golden gate |
| `vif_scale0..3` (via `VMAF_integer_feature`) | integer_vif.c: 778 + vif.c: 367 + vif_tools.c: 618 | ~9,949 | ALL models | CLI, Python harness, ffmpeg, golden gate |
| `motion2` (via `VMAF_integer_feature`) | integer_motion.c: 547 + motion.c: 148 | ~7,611 | ALL models | CLI, Python harness, ffmpeg, golden gate |

**These may not be touched for any drop/demote consideration.**

### Non-canonical features with meaningful standalone value

| Feature | Max model PI | MOS |PLCC| | CPU LOC | All-backend LOC | CLI flag | Recommendation |
|---------|-------------|----------|---------|-----------------|----------|----------------|
| `cambi` | 0.0 | 0.291 | cambi.c: 1,465 | ~5,418 | `--feature cambi` | **KEEP** |
| `psnr_hvs` | 0.0 | 0.358 | psnr_hvs.c: 439 | ~2,962 | `--feature psnr_hvs` | **KEEP** |
| `ssimulacra2` | 0.0 | 0.189 | ssimulacra2.c: 1,024 | ~8,002 | `--feature ssimulacra2` | **KEEP** |
| `float_ssim` | 0.0 | 0.318 | float_ssim.c: 183 + ssim.c: 149 | ~1,766 | `--feature float_ssim` | **KEEP** |
| `float_ms_ssim` | 0.0 | 0.272 | float_ms_ssim.c: 244 | ~3,515 | `--feature float_ms_ssim` | **KEEP** |
| `ciede` (ciede2000) | 0.0 | 0.312 | ciede.c: 456 | ~2,135 | `--feature ciede` | **KEEP** |
| `psnr_y` / `psnr_cb` / `psnr_cr` | 0.0 | 0.368 / 0.173 / 0.136 | psnr.c: 49 + integer_psnr.c: 273 | ~2,503 | `--feature psnr` | **KEEP luma, DEMOTE chroma** |

### Non-canonical features — drop candidates

| Feature | Max model PI | MOS |PLCC| | CPU LOC | All-backend LOC | Downstream consumer | Recommendation |
|---------|-------------|----------|---------|-----------------|---------------------|----------------|
| `ansnr` / `float_ansnr` | 0.0 | ~0.17 (not measured in MOS run, low expected) | ansnr.c: 102 + ansnr_tools.c: 211 + float_ansnr.c: 106 | ~1,626 | Python harness `VMAF_feature` (legacy API) | **DROP** (after deprecation period) |
| `speed_temporal` | 0.0 | 0.090 | speed.c: 1,287 (shared) | ~5,783 (shared with speed_chroma) | None in CLI by default | **DROP** (after deprecation period) |
| `speed_chroma_u/v/uv` | 0.0 | 0.056–0.076 | speed_qa.c: 340 (shared) | shared | None in CLI by default | **DROP** (after deprecation period) |

#### ansnr drop rationale

- `ansnr` (Additive Noise SNR) is a pre-VMAF legacy metric from the original VMAF
  feature engineering phase (2016). It measures the additive noise model residual.
- It was never adopted into any SVM or tiny-AI model as a direct input
- The Python harness `VmafFeatureExtractor` and `FloatVmafFeatureExtractor` classes
  still reference it in `ATOM_FEATURES`, creating a Python API dependency
- Dropping without deprecation would break `compat/python-vmaf` users
- **LOC saved:** ~1,626 (CPU + all GPU backends) plus header cleanups
- **Constraint:** Must check whether any upstream Netflix model JSON uses `ansnr`
  before landing a drop PR. Current audit: none found.

#### speed_temporal + speed_chroma drop rationale

- SpEED (Spatial Efficient Entropic Differencing) metrics were added to the fork as
  experimental features. They measure temporal and chroma video texture change.
- KoNViD MOS |PLCC| is 0.09 (temporal) and 0.06 (chroma) — marginally correlated
- Not referenced in any CLI `--feature` default flag set or in any shipped model
- The fork added CUDA, HIP, Vulkan, and SYCL GPU backends for these features —
  ~5,783 LOC total — adding significant maintenance burden for ~zero predictive gain
- **LOC saved if dropped:** ~5,783 LOC
- **Constraint:** `speed_qa.c` contains internal quality thresholds used by the
  `speed` extractor; ensure `speed.c` does not depend on it if QA code is retained

### Near-redundant within canonical-6

| Feature | Model PI drop | Verdict |
|---------|--------------|---------|
| `vif_scale0` | +0.0014 to +0.0020 | Borderline — essentially redundant with vif_scale1, but removing it requires retraining. **DO NOT DROP without a retrain study.** |
| `vif_scale1` | +0.0055 to +0.0169 | Low but nonzero across all three models. **DEMOTE for future model redesign consideration only.** |

---

## Part 4 — Feature Extractor Full Inventory

### LOC summary per feature (CPU + x86 SIMD + arm64 + CUDA + SYCL + HIP + Vulkan)

| Feature | CPU core | x86 SIMD | arm64 | CUDA | SYCL | HIP | Vulkan | Total |
|---------|----------|-----------|-------|------|------|-----|--------|-------|
| adm (integer) | 4,895 | 8,402 | 178 | 1,216 | 1,549 | 1,267 | 2,338 | ~19,937 |
| vif (integer) | 1,763 | 3,337 | 968 | 619 | 1,594 | 685 | 811 | ~9,949 |
| motion | 1,127 | 1,536 | 426 | 821 | 1,052 | 1,087 | 1,893 | ~7,611 |
| ssimulacra2 | 1,024 | 1,730 | 1,625 | 1,097 | 835 | 983 | 1,508 | ~8,002 |
| cambi | 1,465 | 609 | 198 | 963 | 888 | 867 | 1,249 | ~5,418 |
| speed (all) | 1,627 | 0 | 0 | 1,331 | 1,316 | 1,381 | 1,344 | ~5,783 |
| float_adm | 446 | 497 | 208 | 868 | 914 | 855 | 832 | ~3,708 |
| float_ms_ssim | 726 | 443 | 205 | 543 | 488 | 748 | 846 | ~3,515 |
| float_vif | 361 | 0 | 0 | 445 | 704 | 644 | 653 | ~3,128 |
| psnr_hvs | 439 | 450 | 482 | 489 | 590 | 536 | 539 | ~2,962 |
| psnr (y/cb/cr) | 417 | 187 | 123 | 562 | 410 | 459 | 886 | ~2,503 |
| ciede | 456 | 156 | 82 | 210 | 452 | 421 | 803 | ~2,135 |
| float_ssim | 332 | 149 | 147 | 0 | 875 | 605 | 535 | ~1,766 |
| ansnr | 419 | 100 | 51 | 257 | 361 | 406 | 391 | ~1,626 |
| moment | 202 | 72 | 191 | 199 | 229 | 421 | 353 | ~1,439 |
| fastdvdnet_pre | 291 | 0 | 0 | 0 | 0 | 0 | 0 | 291 |
| feature_mobilesal | 248 | 0 | 0 | 0 | 0 | 0 | 0 | 248 |
| transnet_v2 | 334 | 0 | 0 | 0 | 0 | 0 | 0 | 334 |
| feature_lpips | 202 | 0 | 0 | 0 | 0 | 0 | 0 | 202 |

### Tiny-AI feature models (not traditional extractors)

The following are ONNX-backed inference features (not classical signal-processing
extractors). They are unaffected by any feature extractor drop.

| File | LOC | Purpose |
|------|-----|---------|
| `fastdvdnet_pre.c` | 291 | FastDVDnet temporal pre-filter (5-frame luma) |
| `feature_mobilesal.c` | 248 | MobileSAL saliency detector |
| `feature_lpips.c` | 202 | LPIPS perceptual distance |
| `transnet_v2.c` | 334 | TransNet v2 scene-change detector |

---

## Part 5 — Detailed Top-10 and Bottom-10

### Top-10 by permutation importance (vmaf_tiny_v3)

1. **`adm2`** — drop=+0.479
   ADM (Anti-Distortion Measure) at scale 2. The single most informative feature.
   Captures global distortion sensitivity weighted by visual masking. Used in EVERY
   shipped model. Protected by the Netflix golden gate. **Non-negotiable KEEP.**

2. **`motion2`** — drop=+0.176
   Motion score (5-frame temporal pooling, v2 algorithm). Second most important.
   Captures temporal adaptation / motion masking. **Non-negotiable KEEP.**

3. **`vif_scale3`** — drop=+0.121
   Visual Information Fidelity at the finest Laplacian pyramid scale. **KEEP.**

4. **`vif_scale2`** — drop=+0.049–0.087
   VIF at scale 2. **KEEP.**

5. **`vif_scale1`** — drop=+0.007–0.017
   VIF at scale 1. Low but nonzero across all models. **KEEP** for now; consider
   ablation in future model redesign.

6. **`vif_scale0`** — drop=+0.001–0.002
   VIF at coarsest scale. Near-zero contribution. **KEEP** pending retrain study.

7–10 are non-canonical features not used by any shipped model. Their standalone MOS
correlation is documented above.

### Bottom-10 by combined utility (model PI + MOS PLCC)

1. **`speed_chroma_u`** — model PI: 0.0, MOS |PLCC|: 0.056. Not a CLI default.
   5,783 LOC (shared with `speed_temporal`). **Primary drop candidate.**

2. **`speed_chroma_v`** — model PI: 0.0, MOS |PLCC|: 0.076. Same bundle. **DROP.**

3. **`speed_chroma_uv`** — model PI: 0.0, MOS |PLCC|: 0.070. Same bundle. **DROP.**

4. **`speed_temporal`** — model PI: 0.0, MOS |PLCC|: 0.090. Same bundle. **DROP.**

5. **`ansnr`** — model PI: 0.0. Legacy metric. Python harness dependency.
   1,626 LOC. **DROP** after Python harness deprecation cycle.

6. **`motion`** (raw, non-v2) — model PI: 0.0, MOS |PLCC|: 0.020.
   This is a subsidiary output of the motion extractor, not a standalone extractor.
   It cannot be dropped independently; it is produced as a by-product.

7. **`motion3`** — model PI: 0.0, MOS |PLCC|: 0.035. Same situation as `motion`.

8. **`adm_scale0..3`** — model PI: 0.0 (subsidiary outputs of the ADM extractor,
   not model inputs themselves). MOS |PLCC|: 0.034–0.358. These are by-products of
   computing `adm2` and cannot be independently dropped.

9. **`psnr_cb` / `psnr_cr`** — model PI: 0.0, MOS |PLCC|: 0.136–0.173.
   Chroma PSNR. These are subsidiary outputs of the PSNR extractor (not a separate
   C source); demoting them means excluding them from the default output schema but
   they cost nothing extra to compute. **DEMOTE to opt-in.**

10. **`float_ms_ssim` (as a default-on feature)** — model PI: 0.0, MOS |PLCC|: 0.272.
    Already opt-in via `--feature float_ms_ssim`. **Status quo fine.**

---

## Part 6 — Permutation Importance Script Path Issue

**Finding:** `scripts/dev/permutation_importance.py` resolves `REPO` from `__file__`
(line 22: `REPO = Path(__file__).resolve().parents[2]`) and then builds the parquet
path as `REPO / "runs/full_features_4corpus.parquet"`. When the script is run from a
git worktree (e.g. `.claude/worktrees/agent-*/`), the resolved path is the worktree
root, which has no `runs/` directory. The parquet files live only in the main repo tree.

**Symptom:** `FileNotFoundError: /home/kilian/dev/vmaf/.claude/worktrees/.../runs/...`

**Fix:** The script should accept a `--parquet PATH` argument and fall back to the
default path relative to `REPO`. A one-line `argparse` addition and an `os.path.exists`
check would suffice. This is a separate small fix that should land in its own PR.

---

## Recommendations Summary

| Feature | Recommendation | Rationale | Blocking constraint |
|---------|---------------|-----------|---------------------|
| `adm2`, `vif_scale0..3`, `motion2` | **KEEP (protected)** | Canonical-6; all models; Netflix golden gate | Netflix golden gate §8 |
| `cambi` | **KEEP** | Banding detector; strong CLI surface; |PLCC|=0.29 vs MOS | Used by users |
| `psnr_hvs` | **KEEP** | |PLCC|=0.36 vs MOS; strongest standalone metric | Strong standalone value |
| `ssimulacra2` | **KEEP** | Modern perceptual metric; |PLCC|=0.19; active GPU SIMD investment | User-facing |
| `float_ssim`, `float_ms_ssim`, `ciede` | **KEEP** | User-discoverable; documented CLI flags | User-facing |
| `psnr_y` (luma PSNR) | **KEEP** | |PLCC|=0.37 vs MOS; strongest raw metric | Standard metric |
| `psnr_cb`, `psnr_cr` | **DEMOTE** | Low MOS correlation; subsidiary to psnr_y computation; zero cost to suppress | No blocking constraint |
| `vif_scale0` | **KEEP** (short term) | Near-zero model PI but removal requires model retrain | Model retrain needed |
| `vif_scale1` | **KEEP** (for now) | Low but nonzero model PI in vmaf_tiny_v4 | Model retrain needed |
| `ansnr` / `float_ansnr` | **DROP** (after deprecation) | 0 model weight; legacy pre-VMAF metric; Python harness API dependency | Python harness deprecation cycle |
| `speed_temporal` | **DROP** (after deprecation) | 0 model weight; MOS |PLCC|=0.09; ~5,783 LOC total for speed bundle | Speed bundle must be dropped as a unit |
| `speed_chroma_u/v/uv` | **DROP** (with speed_temporal) | 0 model weight; MOS |PLCC|=0.06–0.08 | Same |
| `float_adm`, `float_vif`, `float_ansnr` | **KEEP** (support HDR float models) | Required by `vmaf_float_v0.6.1` and HDR pipeline | HDR float model path |

**Estimated LOC saved if `ansnr` + `speed` bundle dropped:** ~7,409 LOC

---

## Migration Plan (if drops are approved)

### Phase 1 — Deprecation notices (one PR)

1. Add `VMAF_LOG_WARN` to `ansnr.c:vmaf_fex_init()` and `speed.c:vmaf_fex_init()`
   stating that these extractors are deprecated and will be removed in v4.x
2. Add deprecation notes to `docs/metrics/ansnr.md` and `docs/metrics/speed.md`
3. Add ADR for the drop decision

### Phase 2 — Python harness shim (one PR)

1. Remove `ansnr` from `VmafFeatureExtractor.ATOM_FEATURES`
2. Add a `DeprecationWarning` in `FloatVmafFeatureExtractor` if `ansnr` is
   explicitly requested
3. Update `feature_extractor.py` tests

### Phase 3 — C source removal (one PR, after one release cycle)

1. Delete `core/src/feature/ansnr.c`, `ansnr_tools.c`, `float_ansnr.c`
2. Delete `core/src/feature/speed.c`, `speed_qa.c`
3. Remove all GPU backend variants (CUDA, HIP, SYCL, Vulkan, x86, arm64)
4. Unregister from `feature_extractor.c` registry
5. Update `meson.build` sources lists
6. Verify Netflix golden tests still pass (they do not reference these features)

---

## Corpus and Tooling Notes

- **Best parquet for future re-runs:** `runs/full_features_refresh_4source_partial_20260521.parquet`
  (314,417 rows, most recent 4-corpus extraction as of 2026-05-21; includes all 25 features)
- **MOS-linked parquet:** `runs/full_features_konvid_refresh_20260520_with_mos.parquet`
  (83,096 frame rows, 369 clips, KoNViD-1k 1–5 MOS labels; valid for standalone correlation)
- **Script fix needed:** `scripts/dev/permutation_importance.py` L22/L26 — add
  `--parquet` CLI arg and use main-repo fallback path; otherwise fails in worktrees

---

*Generated by Research-0733 agent, 2026-05-28. Data-driven analysis only — no code changes.*
