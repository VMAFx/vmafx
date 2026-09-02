<!-- markdownlint-disable MD060 -->
# Research: Feature Coverage Audit — 2026-05-18

**Status**: Complete
**ADR**: ADR-0559 (accompanies extraction-script fix PR)
**Author**: Claude Code agent (chore/feature-coverage-audit-and-script-fix)
**Last updated**: 2026-05-18

---

## A. Model Catalog

### SVM JSON models (full-reference, consume libvmaf integer/float features)

All SVM models in `model/` consume the same canonical-6 feature set via
`VMAF_integer_feature_*` or `VMAF_feature_*` keys:

| Model file | Type | Feature set |
|---|---|---|
| `vmaf_v0.6.1.json` | FR SVM (integer) | adm2, motion2, vif_scale0–3 |
| `vmaf_v0.6.1neg.json` | FR SVM (integer) | adm2, motion2, vif_scale0–3 |
| `vmaf_4k_v0.6.1.json` | FR SVM (integer, 4K) | adm2, motion2, vif_scale0–3 |
| `vmaf_4k_v0.6.1neg.json` | FR SVM (integer, 4K) | adm2, motion2, vif_scale0–3 |
| `vmaf_b_v0.6.3.json` | FR SVM bootstrap (integer) | adm2, motion2, vif_scale0–3 |
| `vmaf_float_v0.6.1.json` | FR SVM (float) | adm2, motion2, vif_scale0–3 |
| `vmaf_float_v0.6.1neg.json` | FR SVM (float) | adm2, motion2, vif_scale0–3 |
| `vmaf_float_4k_v0.6.1.json` | FR SVM (float, 4K) | adm2, motion2, vif_scale0–3 |
| `vmaf_float_b_v0.6.3.json` | FR SVM bootstrap (float) | adm2, motion2, vif_scale0–3 |
| `vmaf_rb_v0.6.2/vmaf_rb_v0.6.2.json` | FR SVM residual-bootstrap | adm2, motion2, vif_scale0–3 |
| `vmaf_rb_v0.6.3/vmaf_rb_v0.6.3.json` | FR SVM residual-bootstrap | adm2, motion2, vif_scale0–3 |
| `vmaf_4k_rb_v0.6.2/vmaf_4k_rb_v0.6.2.json` | FR SVM bootstrap (4K) | adm2, motion2, vif_scale0–3 |
| `other_models/nflx_v1.json` | FR SVM (legacy v1) | adm, ansnr, motion, vif (pre-v2 names) |
| `other_models/nflxtrain_norm_type_none.json` | FR SVM (training artifact) | adm2, motion, vif_scale0–3 |
| `other_models/vmaf_v0.6.0.json` | FR SVM v0.6.0 | adm2, motion2, vif_scale0–3 |
| `other_models/vmaf_v0.6.1mfz.json` | FR SVM MFZ variant | adm2 (integer), motion2, vif_scale0–3 |

**No shipped SVM model consumes `speed_chroma` or `speed_temporal`.**

### Tiny-AI ONNX models (fork-added, `model/tiny/`)

| Model | Type | Input features | Model card |
|---|---|---|---|
| `vmaf_tiny_v2.onnx` | FR MLP VMAF surrogate | canonical-6 (adm2, vif_scale0–3, motion2) | `docs/ai/models/vmaf_tiny_v2.md` |
| `vmaf_tiny_v3.onnx` / `.int8` | FR MLP VMAF surrogate | canonical-6 | `docs/ai/models/vmaf_tiny_v3.md` |
| `vmaf_tiny_v4.onnx` / `.int8` | FR MLP VMAF surrogate | canonical-6 | `docs/ai/models/vmaf_tiny_v4.md` |
| `fr_regressor_v1.onnx` | FR MLP (codec-agnostic) | canonical-6 + statistics | `docs/ai/models/fr_regressor_v1.md` |
| `fr_regressor_v2.onnx` / v2 seeds / v3 | FR MLP (codec-aware, ensemble) | canonical-6 + codec one-hot | `docs/ai/models/fr_regressor_v2.md`, `fr_regressor_v3.md` |
| `nr_metric_v1.onnx` / `.int8` | NR quality metric (no reference) | raw luma pixels | `docs/ai/models/nr_metric_v1.md` |
| `konvid_mos_head_v1.onnx` | MOS prediction head (KoNViD) | FULL_FEATURES (21 cols) aggregate | `docs/ai/models/konvid_mos_head_v1.md` (and `model/konvid_mos_head_v1_card.md`) |
| `saliency_student_v1/v2.onnx` | Spatial saliency map | raw frame pixels | `docs/ai/models/saliency_student_v1.md`, `saliency_student_v2_card.md` |
| `learned_filter_v1.onnx` / `.int8` | Frame pre-filter | raw frame pixels | `docs/ai/models/learned_filter_v1.md` |
| `lpips_sq.onnx` | LPIPS perceptual similarity | raw frame pixels | `docs/ai/models/lpips_sq.md` |
| `dists_sq.onnx` | DISTS perceptual similarity | raw frame pixels | `docs/ai/models/dists_sq.md` |
| `mobilesal.onnx` | MobileSal saliency | raw frame pixels | `docs/ai/models/mobilesal.md` |
| `transnet_v2.onnx` | Scene transition detector | raw frame pixels | `docs/ai/models/transnet_v2.md` |
| `fastdvdnet_pre.onnx` | FastDVDnet pre-filter | raw frame pixels | `docs/ai/models/fastdvdnet_pre.md` |
| `vmaf_tiny_v1.onnx` / `v1_medium.onnx` | FR MLP VMAF surrogate (v1) | canonical-6 | (no model card — v1 pre-dates the rule) |
| `predictor_{codec}.onnx` (14 files) | Bitrate predictor per codec | codec+quality input | `model/predictor_{codec}_card.md` |

**No tiny-AI ONNX model currently consumes `speed_chroma` or `speed_temporal`.
The `konvid_mos_head_v1` was trained on FULL_FEATURES (21 cols) which does not
include speed features. The fr_regressor family similarly uses canonical-6.**

---

## B. Feature Extractor Catalog

Source: `core/src/feature/feature_extractor.c` (as of 2026-05-18).

### Float-mode extractors (require `VMAF_FLOAT_FEATURES=1`)

| Extractor | CPU | CUDA | SYCL | Vulkan | HIP | Metal | Notes |
|---|---|---|---|---|---|---|---|
| `float_psnr` | Y | Y | Y | Y | Y | Y | |
| `float_ansnr` | Y | Y | Y | Y | Y | Y | |
| `float_adm` | Y | Y | Y | Y | Y | — | |
| `float_vif` | Y | Y | Y | Y | Y | — | |
| `float_motion` | Y | Y | Y | Y | Y | Y | |
| `float_moment` | Y | Y | Y | Y | Y | Y | |
| **`speed_chroma`** | **Y** | **N** | **N** | **N** | **N** | **N** | **CPU-only** |
| **`speed_temporal`** | **Y** | **N** | **N** | **N** | **N** | **N** | **CPU-only** |

### Integer-mode extractors (default production path)

| Extractor | CPU | CUDA | SYCL | Vulkan | HIP | Metal | Notes |
|---|---|---|---|---|---|---|---|
| `integer_adm` | Y | Y | Y | Y | Y | — | |
| `integer_vif` | Y | Y | Y | Y | Y | — | |
| `integer_motion` | Y | Y | Y | Y | Y | Y | |
| `integer_motion_v2` | Y | Y | Y | Y | Y | Y | |
| `psnr` | Y | Y | Y | Y | Y | Y | |
| `psnr_hvs` | Y | Y | Y | Y | Y | — | |
| `float_ssim` | Y | Y | Y | Y | Y | Y | |
| `float_ms_ssim` | Y | Y | Y | Y | — | Y | |
| `ssim` | Y | — | — | — | — | — | legacy float |
| `ssimulacra2` | Y | Y | Y | Y | Y | — | |
| `ciede` | Y | Y | Y | Y | Y | — | |
| `cambi` | Y | Y | Y | Y | Y | — | |

### Fork-added special extractors (CPU-only)

| Extractor | CPU | GPU | Notes |
|---|---|---|---|
| `speed_qa` | Y | N | SpEED-QA NR metric scaffold (ADR-0253) |
| `lpips` | Y | N | DNN-based, via ONNX Runtime |
| `dists_sq` | Y | N | DNN-based, via ONNX Runtime |
| `fastdvdnet_pre` | Y | N | DNN-based, temporal denoiser pre-filter |
| `mobilesal` | Y | N | DNN-based, saliency |
| `transnet_v2` | Y | N | DNN-based, scene transition |

**GPU twin status for `speed_chroma` and `speed_temporal`**: no GPU twins
exist. Parallel agents (ae53d397645485ccb, acd84cec9116cb626) are porting
them to CUDA/HIP; those stubs are tracked in ADR-0557 and ADR-0558.

---

## C. Extraction Script Feature Sets

### `ai/scripts/chug_extract_features.py`

Uses `FEATURE_SETS` dict from `ai/data/feature_extractor.py`:

- `"canonical"` → DEFAULT_FEATURES (6 features: adm2, vif_scale0–3, motion2)
- `"full"` → FULL_FEATURES (22 features; see below)
- **`speed_chroma` / `speed_temporal`: NOT PRESENT in either set**

### `ai/data/feature_extractor.py` — FULL_FEATURES (22 features)

`adm2, adm_scale0–3, vif_scale0–3, motion, motion2, motion3, psnr_y, psnr_cb,
psnr_cr, float_ssim, float_ms_ssim, cambi, ciede2000, psnr_hvs, ssimulacra2`

Speed features are absent.

### `ai/scripts/extract_full_features.py`

Imports `FULL_FEATURES` from `ai/data/feature_extractor.py` verbatim.
**speed_chroma / speed_temporal: NOT PRESENT.**

### `ai/scripts/bvi_dvc_to_full_features.py`

Hard-codes its own `FULL_FEATURES` tuple (21 features — same as the
`ai/data/feature_extractor.py` version but without `ssimulacra2`).
**speed_chroma / speed_temporal: NOT PRESENT.**

### `ai/scripts/extract_ugc_features.py`

Runs with `CANONICAL_6` only (the 6-feature subset). Does not attempt the
full set.  Schema columns include a full 22-col schema for cross-corpus
compatibility but UGC cells outside canonical-6 are NaN.
**speed_chroma / speed_temporal: NOT PRESENT.**

### `ai/scripts/extract_k150k_features.py`

This script IS already updated (2026-05-15, per inline comment citing
"Lawrence's HDR recipe"). It has:

- `CUDA_CPU_RESIDUAL_EXTRACTOR_NAMES` includes `"speed_temporal"` and
  `"speed_chroma"`.
- `FEATURE_NAMES` (25 cols) includes `speed_temporal`, `speed_chroma_u`,
  `speed_chroma_v`, `speed_chroma_uv`.

**status: speed features ARE added to the K150K script. BUT all rows in the
current corpus (`_reextract_2026-05-17`) show NaN for all speed columns
(5/5 sampled rows = NaN). This indicates either: (a) the re-extract run
that produced the corpus predates or skipped the speed-feature addition, or
(b) the binary used lacked `VMAF_FLOAT_FEATURES` compiled in.**

---

## D. Gap Matrix

### Model × Feature coverage

| Model | adm2 | vif_scale0–3 | motion2 | motion/motion3 | PSNR | SSIM | cambi | ciede | psnr_hvs | ssimulacra2 | **speed_temporal** | **speed_chroma** |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| SVM (vmaf_v0.6.1 family) | Y | Y | Y | — | — | — | — | — | — | — | — | — |
| vmaf_tiny_v2–v4 | Y | Y | Y | — | — | — | — | — | — | — | — | — |
| fr_regressor_v1–v3 | Y | Y | Y | — | — | — | — | — | — | — | — | — |
| konvid_mos_head_v1 | Y | Y | Y | Y | Y | Y | Y | Y | Y | — | **N** | **N** |
| NR / pixel models | — | — | — | — | — | — | — | — | — | — | — | — |

The `konvid_mos_head_v1` was trained on a 21-feature FULL_FEATURES
vector. Speed features were not included in that training set. When the
upstream Netflix HDR model lands and consumes speed features, a new
`mos_head_v2` or `fr_regressor_v4` trained on a speed-inclusive feature
set will be needed.

### Corpus × Script coverage of speed features

| Corpus | Script | speed_temporal | speed_chroma |
|---|---|---|---|
| `.corpus/chug/training/chug_features_partial.jsonl` | `chug_extract_features.py` | **N** (column absent) | **N** |
| `.corpus/chug/training/_reextract_2026-05-17/full_features_chug.rows.jsonl` | `extract_k150k_features.py`-family | column present, **all NaN** | column present, **all NaN** |
| `.corpus/konvid-150k/konvid_150k.jsonl` | `konvid_150k_to_corpus_jsonl.py` | **N** (column absent) | **N** |
| `.corpus/corpus_run/*.jsonl` | (benchmark format, not feature corpus) | N/A | N/A |
| `.corpus/corpus_nvenc/all.jsonl` | (score-only, no feature columns) | N/A | N/A |

---

## E. speed_chroma / speed_temporal — Detailed Status

### Extractor existence

- `core/src/feature/speed.c` (1,566 LoC): confirmed present.
- Registered in `feature_extractor_list[]` under `#if VMAF_FLOAT_FEATURES`.
- Emits 4 feature keys: `Speed_chroma_feature_speed_chroma_u_score`,
  `speed_chroma_v_score`, `speed_chroma_uv_score`, `Speed_temporal_feature_speed_temporal_score`.
- Short aliases registered in `core/src/feature/alias.c`.
- **No GPU twin exists** (CUDA/SYCL/Vulkan/HIP/Metal all absent).

### Script coverage before this PR

| Script | speed_chroma | speed_temporal |
|---|---|---|
| `chug_extract_features.py` | **NO** | **NO** |
| `extract_full_features.py` | **NO** | **NO** |
| `bvi_dvc_to_full_features.py` | **NO** | **NO** |
| `extract_ugc_features.py` | **NO** (intentional: canonical-6 only) | **NO** |
| `extract_k150k_features.py` | YES (added 2026-05-15) | YES (added 2026-05-15) |

### Corpus grep confirmation

Sampled `.corpus/chug/training/chug_features_partial.jsonl` first row keys:
`adm2, clip_name, mos, motion2, saliency_mean, saliency_var,
shot_count_norm, shot_cut_density, shot_mean_len_norm,
vif_scale0–3` — no speed columns.

Sampled `.corpus/chug/training/_reextract_2026-05-17/full_features_chug.rows.jsonl`:
speed columns present but all NaN (5/5 checked). Re-extract required.

### Recommended re-extract scope

1. **CHUG** (highest priority): both `chug_features_partial.jsonl` and
   the 2026-05-17 re-extract need a fresh pass once `speed_chroma` and
   `speed_temporal` are in the extraction script's feature set. The
   reextract agent (a8f22d538ea137ac0) should coordinate.
2. **KoNViD-150k**: `extract_k150k_features.py` already has the speed
   features; however the current corpus JSONL predates their addition
   (confirmed all-NaN). Re-extract the corpus to populate these columns.
3. **Netflix corpus** (via `extract_full_features.py`): add speed features
   to `FULL_FEATURES` in `ai/data/feature_extractor.py` and re-run.
4. **BVI-DVC** (via `bvi_dvc_to_full_features.py`): update the local
   `FULL_FEATURES` tuple and re-run.
5. **UGC** (via `extract_ugc_features.py`): intentionally canonical-6 only;
   no re-extract needed unless the UGC pipeline is upgraded to full features.

---

## F. Netflix HDR Model Surface Check

### Model files

No `vmaf_hdr_*.json` model file exists in the fork's `model/` tree.
The file `model/vmaf_hdr_model_card.md` is explicitly a documentation
placeholder (`.md` extension prevents the resolver from loading it as a
model). Status: documented fallback to SDR model.

### Feature signals in C source

`grep -rn "hdr|HDR|pq|hlg|bt2020|vmaf_hdr|vmaf_v1|vmaf_4k_v1" core/src/`:
results are confined to CAMBI's `luminance_tools.c` / `barten_csf_tools.h`
(HDR perceptual masking math for CAMBI), ARM NEON SSIMULACRA2 luma helpers,
and CUDA/SYCL CAMBI ports. No HDR VMAF model loader or feature-name
references to speed features in the HDR context.

### Upstream model activity

`git log upstream/master -- model/` shows no recent Netflix commits adding
HDR model files. The last model-related commit was `af5f7aa63`
("Add vmaf_4k_v0.6.1neg model"). Issue #645 ("Did the HDR model ever get
released?") was CLOSED 2025-07-25 per research-0089.

### Conclusion

- No HDR VMAF model is shipped or imminent from Netflix upstream.
- The fork has pre-positioned `speed_chroma` and `speed_temporal` as CPU
  extractors (matching the Netflix `speed_ported` branch posture).
- If a future HDR model consumes speed features, the extraction scripts
  (post this PR) will cover them for new corpus runs. Existing corpora
  will need re-extraction.

---

## G. Model Card Audit

| Model | Card location | Feature contract documented? | Stale? |
|---|---|---|---|
| vmaf_tiny_v2 | `docs/ai/models/vmaf_tiny_v2.md` | Yes (canonical-6) | No |
| vmaf_tiny_v3 | `docs/ai/models/vmaf_tiny_v3.md` | Yes (canonical-6) | No |
| vmaf_tiny_v4 | `docs/ai/models/vmaf_tiny_v4.md` | Yes (canonical-6, N=6) | No |
| vmaf_tiny_v5 | `docs/ai/models/vmaf_tiny_v5.md` | — (file exists, check contract) | Unknown |
| fr_regressor_v1 | `docs/ai/models/fr_regressor_v1.md` | Yes | No |
| fr_regressor_v2 | `docs/ai/models/fr_regressor_v2.md` | Yes | No |
| fr_regressor_v3 | `docs/ai/models/fr_regressor_v3.md` | Yes (canonical-6, N=6) | No |
| konvid_mos_head_v1 | `docs/ai/models/konvid_mos_head_v1.md` AND `model/konvid_mos_head_v1_card.md` | Yes (FULL_FEATURES 21-col) | Partially — does not mention speed feature gap |
| nr_metric_v1 | `docs/ai/models/nr_metric_v1.md` | Yes (raw pixels, NR) | No |
| vmaf_tiny_v1 | No card | Feature contract undocumented | **MISSING CARD** |

**Stale / gap flags:**

1. `konvid_mos_head_v1` card does not mention that `speed_chroma` /
   `speed_temporal` are absent from its training feature set. This is
   relevant for future HDR model evaluation.
2. `vmaf_tiny_v1` (and `vmaf_tiny_v1_medium`) have no model card at all.
   These pre-date the ADR-0042 rule; a minimal card should be added.
