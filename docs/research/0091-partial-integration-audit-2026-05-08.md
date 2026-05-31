<!-- markdownlint-disable MD060 -->
# Research-0091: Partial-integration audit — features the AI can't learn from yet

- **Status:** Draft
- **Date:** 2026-05-08
- **Author:** lusoris (with Claude)
- **Tags:** audit, metrics, feature-extractors, tiny-ai, integration, vmaf-tune

## TL;DR

The fork ships **22 user-facing feature extractors** (CPU registry in
`core/src/feature/feature_extractor.c`), most of them with rich SIMD
and GPU-backend coverage. End-to-end integration is not the bottleneck
on the **engine** rungs (1-3). The bottleneck sits squarely on the
**learning** rungs (4-6):

- **Fully integrated, 8 / 8 rungs:** **0** features.
- **Engine-complete (rungs 1+2+3) but AI-blind (rungs 4-6):** **20**
  features. Every metric VMAF actually scores is invisible to the
  fork's predictor / ensemble training because:
  - `tools/vmaf-tune/src/vmaftune/__init__.py:CORPUS_ROW_KEYS` (rung
    4) captures **only `vmaf_score`** — no per-feature columns at all.
    All trainers (`train_fr_regressor_v2.py` line 76-85,
    `train_fr_regressor_v2_ensemble.py` line 82-89,
    `train_fr_regressor_v2_ensemble_loso.py` line 75,
    `train_fr_regressor_v3.py` line 86) require an out-of-band
    "canonical-6" feature subset (`adm2`, `vif_scale0..3`, `motion2`)
    that **the corpus tool does not emit**; the v2 trainer's
    `_row_to_features` (line 282-301) explicitly comments _"Phase A's
    current schema does not emit per-frame features"_ and falls back
    to **synthetic** features in `--synthetic` mode.
  - `tools/vmaf-tune/src/vmaftune/predictor.py:ShotFeatures` (lines
    53-86) accepts **zero** libvmaf metric outputs — only probe-encode
    bytes, saliency mean/var, signalstats luma stats, and structural
    metadata. Rung 6 fails for every metric without exception.
- **Backend-incomplete on rung 2:** **5** notable cases (CAMBI is
  Vulkan-only on GPUs; SSIM-fixed CPU is dead code; HIP coverage gaps
  on ADM / VIF / SSIM; SYCL has no CAMBI / SSIMULACRA2 has no NEON
  SIMD).
- **Doc-incomplete on rung 7:** **2** features missing dedicated pages
  (CIEDE2000 has no `docs/metrics/ciede.md`; ANSNR has no own page).
  `docs/metrics/features.md` is the per-feature reference and covers
  every shipped extractor inline; standalone pages exist only for
  CAMBI today.
- **Engine-broken (rung 1):** integer-fixed `ssim` defines
  `vmaf_fex_ssim` in `core/src/feature/integer_ssim.c:280` but is
  **never registered** in `feature_extractor_list[]`
  (`feature_extractor.c` lines 145-225). The symbol is dead — the only
  CPU SSIM that actually runs is `float_ssim`. The
  `docs/metrics/features.md` table even lists `ssim` (fixed) as
  shipping with a Vulkan kernel and "no SIMD"; the row is misleading
  because `--feature ssim` cannot resolve at all.
- **Engine-broken (rung 1, milder):** integer-fixed PSNR-HVS has CUDA
  / SYCL / Vulkan kernels but no CPU file (`integer_psnr_hvs.c`
  doesn't exist; only `integer_psnr_hvs_cuda.c`,
  `integer_psnr_hvs_sycl.cpp`, `psnr_hvs_vulkan.c`); the CPU
  `psnr_hvs` extractor at `core/src/feature/psnr.c` (no — see
  inline note in `features.md`) covers it via a different path.

The single highest-ROI gap for the user's stated framing ("we can only
learn from them with AI"): **add per-feature columns to
`CORPUS_ROW_KEYS`**. Any trainer that wants to consume more than
`adm2 + vif_scale0..3 + motion2` is currently blocked at the data
layer, not at the model layer.

## End-to-end integration ladder used

Each row in the matrix below is scored against eight rungs:

1. **CPU reference** — `.c` file under `core/src/feature/`, registered in
   `feature_extractor_list[]`.
2. **All shipped backends covered** — CUDA + SYCL + Vulkan + HIP, where
   each is meaningful.
3. **SIMD coverage** — AVX2 + AVX-512 + NEON for pixel-level features.
4. **Corpus row schema** — the feature's per-frame value lands in
   `CORPUS_ROW_KEYS`.
5. **Trainer feature input** — the feature is consumed as a column by
   `ai/scripts/train_fr_regressor_v2*.py` / `_v3.py` / ensemble.
6. **Predictor input** — the feature surfaces in
   `tools/vmaf-tune/src/vmaftune/predictor.py:ShotFeatures`.
7. **User-facing surface** — documented in `docs/metrics/`, exposed via
   `--feature <name>` on the CLI, available via the ffmpeg `libvmaf`
   filter.
8. **Tests** — Netflix-golden CPU + at least one cross-backend ULP
   (the `/cross-backend-diff` artefact).

Legend: ✅ full, ⚠️ partial, ❌ missing, – not applicable.

## Coverage matrix

| Feature             | Invocation        | CPU | CUDA | SYCL | Vulkan | HIP | SIMD | Corpus | Trainer | Predictor | Docs | Tests | Score |
|---------------------|-------------------|-----|------|------|--------|-----|------|--------|---------|-----------|------|-------|-------|
| VIF (fixed)         | `vif`             | ✅   | ✅    | ✅    | ✅      | ⚠️¹ | ✅    | ❌      | ✅       | ❌         | ✅    | ✅     | 9 / 12 |
| VIF (float)         | `float_vif`       | ✅   | ✅    | ✅    | ✅      | ⚠️¹ | –    | ❌      | ⚠️²      | ❌         | ✅    | ✅     | 8 / 12 |
| Motion (fixed)      | `motion`          | ✅   | ✅    | ⚠️³  | ✅      | ⚠️¹ | ✅    | ❌      | ✅       | ❌         | ✅    | ✅     | 8 / 12 |
| Motion v2 (fixed)   | `motion_v2`       | ✅   | ✅    | ✅    | ✅      | ✅   | ✅    | ❌      | ⚠️²      | ❌         | ✅    | ✅     | 9 / 12 |
| Motion (float)      | `float_motion`    | ✅   | ✅    | ✅    | ✅      | ✅   | ✅    | ❌      | ⚠️²      | ❌         | ✅    | ✅     | 9 / 12 |
| ADM (fixed)         | `adm`             | ✅   | ✅    | ✅    | ✅      | ⚠️¹ | ✅    | ❌      | ✅       | ❌         | ✅    | ✅     | 9 / 12 |
| ADM (float)         | `float_adm`       | ✅   | ✅    | ✅    | ✅      | ❌⁴  | ✅    | ❌      | ⚠️²      | ❌         | ✅    | ✅     | 8 / 12 |
| CAMBI               | `cambi`           | ✅   | ❌    | ❌    | ✅      | ❌   | ⚠️⁵  | ❌      | ❌       | ❌         | ✅    | ✅     | 5 / 12 |
| CIEDE2000           | `ciede`           | ✅   | ✅    | ✅    | ✅      | ✅   | ✅    | ❌      | ❌       | ❌         | ⚠️⁶  | ✅     | 8 / 12 |
| PSNR (fixed)        | `psnr`            | ✅   | ✅    | ✅    | ✅      | ✅   | ✅    | ❌      | ❌       | ❌         | ✅    | ✅     | 8 / 12 |
| PSNR (float)        | `float_psnr`      | ✅   | ✅    | ✅    | ✅      | ✅   | ✅    | ❌      | ❌       | ❌         | ✅    | ✅     | 8 / 12 |
| PSNR-HVS            | `psnr_hvs`        | ✅   | ✅    | ✅    | ✅      | ❌⁴  | ⚠️⁷  | ❌      | ❌       | ❌         | ✅    | ✅     | 7 / 12 |
| SSIM (fixed)        | `ssim`            | ❌⁸  | –    | –    | ✅      | –   | –    | ❌      | ❌       | ❌         | ⚠️⁸  | ❌     | 1 / 12 |
| SSIM (float)        | `float_ssim`      | ✅   | ✅    | ✅    | ✅      | ✅   | ✅    | ❌      | ❌       | ❌         | ✅    | ✅     | 8 / 12 |
| MS-SSIM (float)     | `float_ms_ssim`   | ✅   | ✅    | ✅    | ✅      | ❌⁴  | ✅    | ❌      | ❌       | ❌         | ✅    | ✅     | 7 / 12 |
| ANSNR (float)       | `float_ansnr`     | ✅   | ✅    | ✅    | ✅      | ✅   | –    | ❌      | ❌       | ❌         | ⚠️⁶  | ❌     | 6 / 12 |
| SSIMULACRA2         | `ssimulacra2`     | ✅   | ✅    | ✅    | ✅      | ❌⁴  | ✅    | ❌      | ❌       | ❌         | ✅    | ✅     | 7 / 12 |
| Float moment        | `float_moment`    | ✅   | ✅    | ✅    | ✅      | ✅   | ⚠️⁹  | ❌      | ❌       | ❌         | ✅    | ❌     | 6 / 12 |
| LPIPS (tiny-AI)     | `lpips`           | ✅   | –ᴬ   | –ᴬ   | –ᴬ     | –ᴬ  | –    | ❌      | ❌       | ❌         | ✅    | ❌     | 4 / 12 |
| FastDVDnet pre      | `fastdvdnet_pre`  | ✅   | –    | –    | –      | –   | –    | ❌      | ❌       | ❌         | ✅    | ✅     | 3 / 12 |
| Mobilesal           | (internal)        | ✅   | –    | –    | –      | –   | –    | ❌      | ❌       | ❌         | ✅    | ✅     | 3 / 12 |
| TransNet V2         | `transnet_v2`     | ✅   | –    | –    | –      | –   | –    | ❌      | ❌       | ❌         | ✅    | ✅     | 3 / 12 |
| Speed chroma        | `speed_chroma`    | ✅   | –    | –    | –      | –   | –    | ❌      | ❌       | ❌         | ✅    | ✅     | 3 / 12 |
| Speed temporal      | `speed_temporal`  | ✅   | –    | –    | –      | –   | –    | ❌      | ❌       | ❌         | ✅    | ✅     | 3 / 12 |
| Saliency-student    | (python only)     | ❌ᴮ | –    | –    | –      | –   | –    | ❌      | ❌       | ⚠️ᴮ       | ✅    | ⚠️    | 1 / 12 |

### Footnotes

¹ HIP exposes `ciede`, `motion`, `vif`, `adm` via the C-side `_hip.c`
files in `core/src/feature/hip/` but several are still labelled "HIP
nth-consumer scaffold" (T7-10a/b series, ADR-0273/0274) — kernels run
under `hipLaunchKernelGGL` but several sibling agents (`feat/hip-*`
worktrees) are completing the bit-exact-vs-CUDA gate. Treat ⚠️ as
"compiles + registers + emits scores; cross-backend ULP not yet at the
T6-8 GPU-parity bar (ADR-0214)". CI-side this is the same status
tracked in [`docs/state.md`](../state.md).

² Trainer consumes `motion2`, `vif_scale0..3`, `adm2` via
`CANONICAL_6` — the columns are sourced **from a non-canonical
sidecar JSON the corpus tool does not produce**. So even the
"trainer-fed" features are only consumed when an external pipeline
hand-attaches `per_frame_features` to corpus rows. v3 raises the bar:
`train_fr_regressor_v3.py:143` requires the columns inside the corpus
DataFrame, which the current `corpus.py` cannot satisfy.

³ Motion (fixed) on SYCL: `integer_motion_sycl.cpp` exists and is
registered, but `motion2` GPU-vs-CPU bit-exactness is one of the
recurring `/cross-backend-diff` debt items (see ADR-0186 +
`docs/state.md` SYCL section).

⁴ HIP coverage gaps: `float_adm`, `psnr_hvs`, `float_ms_ssim`,
`ssimulacra2` have no `_hip.c` file in `core/src/feature/hip/`.
T7-10b consumer plan (ADR-0273) tracks the rollout; not all 8
consumers have landed.

⁵ CAMBI SIMD: `cambi_avx2.c` and `cambi_avx512.c` exist
(`core/src/feature/x86/`) and `cambi_neon.c`
(`core/src/feature/arm64/`) — but `docs/metrics/features.md`'s
table reports "—" for CAMBI SIMD. Either the doc is stale or the
SIMD paths exist but are not runtime-dispatched. Needs verification.

⁶ CIEDE2000 and ANSNR have no dedicated `docs/metrics/<name>.md`
page. They are documented inline in `docs/metrics/features.md` under
the per-extractor table, which satisfies ADR-0100's per-surface
minimum bar (invocation / output / range / formats / limitations) —
but the "every shipped feature has a page" framing in CLAUDE.md §12
r10 is debatable; CAMBI is the only fork-added metric with its own
file. ⚠️ rather than ❌ because the inline coverage exists.

⁷ PSNR-HVS NEON: `psnr_hvs_neon.c` exists; AVX-512 path does **not**
exist (`features.md` correctly says "AVX2, NEON" only). ⚠️ because
the AVX-512 gap is intentional per ADR-pending; it is not
"incomplete" so much as "deliberately scalar on AVX-512".

⁸ **SSIM (fixed) is dead.** `integer_ssim.c:280` defines
`VmafFeatureExtractor vmaf_fex_ssim` but `feature_extractor_list[]`
in `core/src/feature/feature_extractor.c:145-225` does **not**
include `&vmaf_fex_ssim`. The Vulkan kernel
(`ssim_vulkan.c`) and the doc row in `docs/metrics/features.md` both
imply the feature ships, but `--feature ssim` cannot resolve at the
CLI because the symbol is unreachable from the registry. Either
(a) register it, or (b) delete the file + fix docs. A grep for any
other reference to `vmaf_fex_ssim` returns only the definition
itself.

⁹ Float moment SIMD: `moment_avx2.c` and `moment_neon.c` ship
(`core/src/feature/x86/`, `core/src/feature/arm64/`); AVX-512 is
absent (matches `features.md` "AVX2, NEON" row).

ᴬ LPIPS dispatches through ORT's execution provider (CPU / CUDA /
OpenVINO / ROCm). It does not own a libvmaf-side GPU kernel; that's
by design (the GPU-ness is delegated to ONNX Runtime). Marked – not
❌.

ᴮ Saliency-student is **not** a libvmaf feature extractor at all. It
is registered as a tiny-AI model (`docs/ai/models/saliency_student_v1.md`),
exposed only through `tools/vmaf-tune/src/vmaftune/saliency.py`, and
its outputs surface in `ShotFeatures.saliency_mean` /
`saliency_var` — making it the **only** rung-6-positive integration
on the matrix. Listed for completeness because the user named it; not
a libvmaf shipped surface, so the "CPU / CUDA / SYCL …" rungs are not
applicable.

### Needs verification

- CAMBI SIMD — `cambi_avx2.c` etc. exist as files; runtime dispatch
  needs confirmation against `cpu.c` to confirm whether they're wired
  through (footnote ⁵). Marked ⚠️ pending.
- HIP "scaffold-vs-shipped" status per kernel — CI status under
  `gh workflow` is the source of truth; this audit treats the
  presence of a `_hip.c` + registry entry as ✅ but flags the
  T7-10b consumer-plan items as ⚠️ where the cross-backend ULP gate
  has not yet been declared green for that feature.

## High-priority promotions (AI-relevant — rungs 4-6)

The user's central concern: features that score well on the engine
rungs (1-3) but where the AI cannot learn from them because rung 4
(corpus row schema) is the bottleneck.

**Top 5 promotions ranked by AI-stack ROI:**

### 1. Add per-feature columns to `CORPUS_ROW_KEYS`

- **What's missing:** `tools/vmaf-tune/src/vmaftune/__init__.py:26-53`
  has 26 keys, none of them per-feature. Per-frame VMAF feature values
  are produced by `libvmaf` on every score run and discarded.
- **Suggested next PR:** Add `per_frame_features: dict[str, list[float]]`
  (or a flattened `feature_<name>_mean` / `_p10` / `_p90` /
  `_var` quartet per feature) to the corpus row. Bump
  `SCHEMA_VERSION` from 2 to 3. Update `corpus.py` to capture from
  the libvmaf JSON output (the CLI already writes per-frame metrics
  via the `--json` flag).
- **One-line ROI:** Unblocks every existing trainer (v2, v2-ensemble,
  v2-ensemble-loso, v3) to consume real per-frame canonical-6 instead
  of synthetic placeholders. Also opens rung 5 for non-canonical
  features (psnr/ssim/cambi/ssimulacra2/ciede) via downstream PRs.

### 2. Extend `ShotFeatures` to accept libvmaf metric digest

- **What's missing:**
  `tools/vmaf-tune/src/vmaftune/predictor.py:53-86` accepts only
  probe-encode bytes + saliency + signalstats luma. Zero perceptual
  metrics from libvmaf reach the predictor MLP.
- **Suggested next PR:** Add three new fields per metric of interest:
  `metric_<name>_mean` / `_var` / `_p10` (the predictor wants
  cheap-to-compute summary stats, not full per-frame). Start with
  `cambi_mean`, `psnr_y_mean`, `ssimulacra2_mean` — the three metrics
  most-likely to improve VMAF prediction on out-of-distribution content
  (banding, severe-degradation, perceptual-jpeg-style).
- **One-line ROI:** Closes rung 6 for the highest-signal metrics; gives
  the predictor a learnable tail-quality prior beyond the canonical-6.

### 3. Promote canonical-6 to v4: add `cambi` + `ssimulacra2`

- **What's missing:** Trainers v2/v3 hard-code 6 features; CAMBI and
  SSIMULACRA2 are not consumed despite both being shipped extractors
  with high perceptual-quality signal that VMAF (the model) does not
  fuse natively.
- **Suggested next PR:** Define `CANONICAL_8` as a v4 schema in a
  new ADR, add `cambi` + `ssimulacra2` to the trainer's input columns,
  retrain `fr_regressor_v3` against an extended-corpus run.
- **One-line ROI:** Closes rung 5 for two high-signal metrics that
  the fork ships fully but the AI ignores. Banding-sensitive content
  in particular loses the most signal today.

### 4. Wire saliency into the corpus row

- **What's missing:** `saliency_student_v1` runs only inside
  `vmaftune/saliency.py` and feeds the **predictor** but never lands
  in the **corpus**. The trainer cannot use it for ground-truth
  conditioning.
- **Suggested next PR:** Add `saliency_mean` + `saliency_var` to
  `CORPUS_ROW_KEYS`; have `corpus.py` invoke the saliency-student
  ONNX on the centre frame at corpus-build time. Schema bump to v4.
- **One-line ROI:** The fork's only rung-6-shipped feature becomes
  rung-5-shipped too — closes the loop so the regressor can
  condition on the same saliency signal the predictor uses.

### 5. SSIM-fixed: register or delete

- **What's missing:** `vmaf_fex_ssim` is defined but unregistered
  (footnote ⁸). Either path is a small PR; the current state is
  "engine-broken" for the only feature the doc table claims ships
  but cannot resolve from the CLI.
- **Suggested next PR:** Decide via ADR. Either (a) register the
  symbol and add a `test_ssim_fixed.c` smoke test, or (b) delete
  `integer_ssim.c` + `integer_ssim_cuda.c` + the Vulkan / SYCL
  twins, and remove the row from `features.md`. (b) is the lower-
  effort path; the float SSIM covers the same use case.
- **One-line ROI:** Stops the doc-vs-code drift that future
  contributors will hit. Not directly an AI-stack item; included
  because it's a rung-1 break that surfaced during this audit.

## Backend-incomplete (rung 2)

For completeness — these are not the AI-blockers but they're the
"the fork ships GPU backends X but feature F is missing from one
of them" debt:

| Feature        | Missing on        | Tracking              |
|----------------|-------------------|-----------------------|
| CAMBI          | CUDA, SYCL, HIP   | ADR-0210 (Vulkan only) |
| `float_adm`    | HIP               | T7-10b consumer plan  |
| `psnr_hvs`     | HIP               | T7-10b consumer plan  |
| `float_ms_ssim`| HIP               | T7-10b consumer plan  |
| `ssimulacra2`  | HIP               | T7-10b consumer plan  |
| `motion2` (SYCL)| ULP-gate pending | ADR-0186              |
| SSIM-fixed     | every backend (dead symbol) | this audit |

The HIP gaps are tracked sibling-agent work (`feat/hip-*-consumers`
worktrees); not a fresh action item. CAMBI on CUDA / SYCL is real
debt — it's the only metric on the matrix with three GPU-backend
gaps at once.

## Doc-incomplete (rung 7)

`docs/metrics/features.md` provides per-extractor coverage inline
that satisfies ADR-0100's minimum bar (invocation, output keys,
output range, input formats, options, backends, limitations) for
all shipped extractors. Standalone files exist only for CAMBI today.

Per ADR-0100's per-surface bar, **inline coverage in features.md is
acceptable**; the rule does not require a separate `.md` per metric.
This audit therefore marks rung 7 as ✅ where features.md is
substantive and ⚠️ where the inline section is thin (CIEDE2000 and
ANSNR get only a one-liner table row plus footnotes).

**Tiny-AI stricter bar (ADR-0042 5-point):** The five tiny-AI
features below are required to ship the 5-point doc per ADR-0042
(model card, inputs, outputs, limitations, retraining recipe). All
five have a `docs/ai/models/<name>.md` page; quality varies but
none are missing a page.

| Tiny-AI feature          | Page                                       | 5-point bar status              |
|--------------------------|--------------------------------------------|---------------------------------|
| `lpips`                  | `docs/ai/models/lpips_sq.md`               | Verify; not part of this audit  |
| `fastdvdnet_pre`         | `docs/ai/models/fastdvdnet_pre.md`         | Verify; not part of this audit  |
| `mobilesal` (internal)   | `docs/ai/models/mobilesal.md`              | Verify; not part of this audit  |
| `transnet_v2`            | `docs/ai/models/transnet_v2.md`            | Verify; not part of this audit  |
| `saliency_student_v1`    | `docs/ai/models/saliency_student_v1.md`    | Verify; not part of this audit  |

5-point compliance is out of scope for this digest — covered by the
sibling Phase-A-promotion audit (`af3bb1432deaf63ad`) and the tiny-AI
SOTA web-research strand.

## Test-incomplete (rung 8)

- `float_moment` — no `test_moment.c`; only `test_moment_simd.c`
  covers the SIMD path. Netflix-golden coverage absent.
- `float_ansnr` — no dedicated test file under `core/test/`.
- `lpips`, `mobilesal`, `transnet_v2`, `fastdvdnet_pre` —
  `test_mobilesal.c`, `test_transnet_v2.c`, `test_fastdvdnet_pre.c`
  exist; `test_lpips.c` does not. LPIPS is engine-tested via the
  ORT smoke (`core/test/dnn/`) but lacks a Netflix-golden row.
- `ssim` (fixed) — no test, dead symbol.
- Saliency-student — Python-side tests exist
  (`tools/vmaf-tune/tests/`); not a libvmaf C test.

## Recommended sprint plan

Ranked by AI-stack ROI; each item is a single PR scope.

1. **Schema v3 — per-feature corpus columns** (rung 4).
   - File: `tools/vmaf-tune/src/vmaftune/__init__.py`,
     `tools/vmaf-tune/src/vmaftune/corpus.py`,
     `ai/scripts/train_fr_regressor_v2.py:_row_to_features`.
   - Scope: ~150-300 LOC; bump `SCHEMA_VERSION` to 3; emit per-frame
     features array from the libvmaf JSON parser; update v2/v3
     trainers to consume the schema directly without sidecar.
   - ADR required (schema change).

2. **Predictor input expansion — `ShotFeatures` v2** (rung 6).
   - File: `tools/vmaf-tune/src/vmaftune/predictor.py`,
     `tools/vmaf-tune/src/vmaftune/predictor_features.py`.
   - Scope: ~200-400 LOC; add metric-digest fields, retrain
     analytical-fallback coefficients, update predictor smoke tests.
   - ADR required (predictor input contract change).

3. **CANONICAL_8 — add cambi + ssimulacra2 to v4 trainer** (rung 5).
   - File: `ai/scripts/train_fr_regressor_v4.py` (new).
   - Depends on item 1.
   - ADR required (input vocabulary expansion).

4. **Saliency in corpus** (rung 5).
   - File: `tools/vmaf-tune/src/vmaftune/corpus.py`,
     `tools/vmaf-tune/src/vmaftune/saliency.py`.
   - Depends on item 1; piggy-backs on the schema bump.

5. **SSIM-fixed dead-symbol cleanup** (rung 1).
   - File: `core/src/feature/feature_extractor.c` (register) **or**
     delete the integer SSIM family. Quick ADR either way.
   - Touched-file cleanup per ADR-0141.

## References

- ADR-0042 — tiny-AI docs required per PR (5-point bar).
- ADR-0100 — project-wide doc substance rule.
- ADR-0141 — touched-file cleanup rule.
- ADR-0186 — Vulkan image-import impl + cross-backend bit-exactness
  posture.
- ADR-0210 — CAMBI Vulkan integration (the source of CAMBI's
  Vulkan-only GPU posture).
- ADR-0214 — T6-8 GPU-parity gate.
- ADR-0237 — quality-aware encode automation roadmap (Phase A → F).
- ADR-0273 / ADR-0274 — HIP T7-10b consumer plan.
- ADR-0291 / ADR-0319 — canonical-6 trainer feature contract.
- ADR-0297 — sample-clip mode (origin of `clip_mode` in
  `CORPUS_ROW_KEYS` v2).
- Research-0044 — quality-aware encode automation option-space.
- Research-0078 — encoder-vocab v3 schema expansion (sibling).
- Research-0081 — fr_regressor_v2 ensemble real-corpus methodology.
- Research-0084 — ffmpeg-patch vmaf-tune integration survey.
- Phase-A-promotion audit (in flight, worktree
  `agent-af3bb1432deaf63ad`) — different angle (scaffold markers
  vs integration completeness); cross-link.
- Phase-F design (sibling worktree
  `agent-ad1f149047faaf0db`) — composition of existing surfaces;
  this audit's findings inform Phase F's "what's actually
  composable" inventory.
- Tiny-AI SOTA web research (sibling) — external SOTA comparison.
- Predictor train pipeline (sibling) — companion to item 2 above.

## Appendix: source-of-truth file paths

- CPU registration: `core/src/feature/feature_extractor.c:145`
  (the `feature_extractor_list[]` table).
- Corpus schema: `tools/vmaf-tune/src/vmaftune/__init__.py:22-53`
  (`SCHEMA_VERSION` and `CORPUS_ROW_KEYS`).
- v2 trainer canonical-6: `ai/scripts/train_fr_regressor_v2.py:78-85`.
- v2 trainer "no per-frame" comment:
  `ai/scripts/train_fr_regressor_v2.py:298-301`.
- v3 trainer required-columns assertion:
  `ai/scripts/train_fr_regressor_v3.py:143-148`.
- Predictor `ShotFeatures`:
  `tools/vmaf-tune/src/vmaftune/predictor.py:53-86`.
- Dead `vmaf_fex_ssim` definition:
  `core/src/feature/integer_ssim.c:280`.
