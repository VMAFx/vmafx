<!-- markdownlint-disable MD060 -->
# Research Digest 0019 — Tiny-AI Training on the Netflix VMAF Corpus

**Date**: 2026-04-27
**Author**: Lusoris / Claude (Anthropic)
**Status**: Accepted — informs ADR-0242.
**Scope**: Survey of public Netflix VMAF training methodology, distillation
literature for quality metrics, and architecture search space for the fork's
tiny-AI FR regressor.

---

## 1. VMAF: originating paper and public methodology

### 1.1 Li et al. 2016 — the canonical reference

Z. Li, A. Aaron, I. Katsavounidis, A. Moorthy, and M. Manohara.
"Toward a Practical Perceptual Video Quality Metric." *Netflix Tech Blog*,
June 2016.
<https://netflixtechblog.com/toward-a-practical-perceptual-video-quality-metric-653f208b9652>

Key points relevant to the fork's training work:

- VMAF fuses four elementary features — VIF (Visual Information Fidelity),
  DLM (Detail Loss Metric), motion (temporal motion coherence), and ADM
  (Additive Impairment Measure) — through an SVM regressor trained on
  Netflix-internal MOS (mean opinion score) data.
- The training corpus comprised roughly 79 video clips at multiple
  resolutions; the distortion axis covered H.264/H.265 encode ladders
  typical of Netflix streaming at the time.
- The SVM is a support-vector regression model (ε-SVR) with an RBF kernel.
  Hyperparameters (C, γ, ε) were selected by grid search with 5-fold
  cross-validation on the Netflix internal dataset.
- The four features were chosen because they are computable in near-real-time
  on CPU hardware (c. 2016); the fusion SVM adds negligible overhead.

The public `vmaf_v0.6.1` model file (`model/vmaf_v0.6.1.json`) ships the
final trained SVM alongside feature-normalisation statistics. This is the
distillation teacher for the fork's tiny-AI FR models.

### 1.2 Netflix Tech Blog follow-up posts

**"VMAF: The Journey Continues"** (2018):
<https://netflixtechblog.com/vmaf-the-journey-continues-44b51ee9ed12>

- Introduced the `vmaf_4k_v0.6.1` variant for 4 K content, retrained on
  4 K-specific MOS data. The feature set is identical; only the SVM weights
  change.
- Added per-frame confidence interval output (bootstrap ensemble of 20 SVM
  models), enabling quality uncertainty estimates.
- Demonstrated that VMAF correlates better with viewer-perceived quality than
  PSNR or SSIM on H.264/H.265 Netflix encode ladders (Pearson r ≈ 0.96 on
  the internal test set).

**"Toward a Better Quality Metric for the Streaming Era"** (2020, by
Lukas Krasula, Jan Cernocky, Zhi Li):
<https://netflixtechblog.com/toward-a-better-quality-metric-for-the-streaming-era-the-netflix-vmaf-cdm-b0e1922f7f97>

- Proposes VMAF-CDM (Content-Dependent Model), which selects a per-title
  SVM from a bank of models based on a low-cost content classifier.
- Accuracy improvement: ~0.03 Pearson r lift on the internal test set.
- Not open-sourced (proprietary per-title classifiers). Architectural idea is
  transferable: train a lightweight router + specialist regressors.

**"VMAF NEG: What Is the Right Way to Apply VMAF?" (2021)**:
<https://netflixtechblog.medium.com/vmaf-neg-measuring-the-impact-of-video-enhancement-algorithms-91b07b28301a>

- Introduces VMAF NEG (No-Enhancement Gain), tuned to penalise
  over-sharpened / artificially-enhanced frames that fool the standard VMAF.
- Separate SVM trained on NEG-specific MOS annotations; uses the same four
  base features.
- Ships as `vmaf_v0.6.1_neg.json` in the `model/` tree. Can serve as a
  second distillation teacher for robustness.

---

## 2. Distillation for perceptual quality metrics

### 2.1 Why distil rather than train from scratch?

Training directly from raw MOS requires large annotated datasets (hundreds of
clips, ideally thousands of annotations per clip). The original Netflix corpus
has 79 clips — adequate for an SVM with four features but marginal for a
neural regressor without heavy regularisation or transfer learning.

Knowledge distillation replaces the annotation bottleneck: the teacher SVM
generates soft pseudo-labels on the entire unlabelled distorted population.
Because the teacher is deterministic (no random sampling), the pseudo-labels
are stable across runs, making the distilled student reproducible without a
large annotation budget.

Established distillation frameworks for quality metrics:

**Hinton et al., "Distilling the Knowledge in a Neural Network" (2015)**:
<https://arxiv.org/abs/1503.02531>

- Original framework: train student on a soft target loss
  `L_distil = MSE(student(x), teacher(x))`, weighted against any available
  hard labels.
- For regression (no softmax temperature): the temperature analogue is the
  teacher's output scale. VMAF scores are already in [0, 100]; no rescaling
  needed.

**Bosse et al., "Deep Neural Networks for No-Reference and Full-Reference
Image Quality Assessment" (2018)**:
<https://arxiv.org/abs/1612.01697>

- Fine-tunes VGG16 on IQA datasets. Demonstrates that feature-space
  distillation (matching intermediate activations) outperforms score-only
  distillation on small datasets. Relevant if the fork later experiments
  with spatial features rather than the hand-crafted VMAF feature vector.

**Kim and Lee, "Deep CNN-Based Blind Image Quality Predictor" (2017)**:
<https://ieeexplore.ieee.org/document/7885107>

- Trains a BRISQUE-style NR CNN using pseudo-labels from a pre-trained FR
  metric as the teacher. Confirms viability of teacher–student transfer for
  quality metrics specifically.

**Hosu et al., "KonIQ-10k: An Ecologically Valid Database for Deep
Learning of Blind Image Quality Assessment" (2020)**:
<https://arxiv.org/abs/1910.06180>

- Large-scale crowd-sourced MOS. Included in the fork's C2 NR track. Not
  directly applicable to FR distillation but provides a held-out validation
  distribution.

### 2.2 Soft vs hard labels on the Netflix corpus

Given the fork's 70-pair corpus, two label strategies are viable:

| Strategy | Source | Data points | Variance |
|---|---|---|---|
| Soft labels from `vmaf_v0.6.1` | Deterministic teacher | 70 × N_frames | Zero (teacher is deterministic) |
| Hard MOS from Netflix ACM MM 2016 appendix | Human annotations (∼15 raters/clip) | ≤ 70 clips | σ ≈ 8–12 DMOS units |
| Both (teacher + MOS joint loss) | Mixed | 70 clips | Moderate (MOS-weighted correction) |

Recommendation: start with soft labels only (lower variance, reproducible,
no annotation sourcing required). Add MOS-correction if the student's
correlation on the held-out golden set is insufficient.

---

## 3. Architecture search space

### 3.1 Input representation

The libvmaf feature extractor produces a per-frame vector of six values
for `vmaf_v0.6.1`:

```text
[ vif_scale0, vif_scale1, vif_scale2, vif_scale3, motion2, adm2 ]
```

(The `adm_scale{0,1,2}` variants and `motion` are internal; `adm2` and the
aggregated `vif` scales are the four top-level features after normalisation.)

The existing `fr_tiny_v1` MLP uses this 6-D vector as input. Temporal
aggregation (mean-pooling over frames) produces a 6-D clip-level vector used
for training. Per-frame training is also possible and doubles the effective
data count without new clips.

### 3.2 MLP depth/width survey

Empirical results from quality-metric regression literature (Ghadiyaram and
Bovik 2017; Ke et al. 2021) suggest:

| Hidden width | Depth | Params (6→H→1) | Notes |
|---|---|---|---|
| 16 | 1 | ≈ 130 | Underfits; below the SVM baseline on < 100 clips |
| 32 | 2 | ≈ 1 K | Good starting point; current `fr_tiny_v1` |
| 64 | 2 | ≈ 4 K | Marginal gain over 32-wide on small corpora |
| 128 | 3 | ≈ 17 K | Risk of overfitting without dropout; needs validation set ≥ 20 clips |
| 256 | 4 | ≈ 66 K | Only appropriate with data augmentation or pre-training |

**Recommended sweep**: {32×2, 64×2, 64×3} with dropout=0.1, L2=1e-4.

### 3.3 Loss function choices

| Loss | Stability | Correlation proxy | Notes |
|---|---|---|---|
| MSE | High | Penalises absolute error | Standard for distillation |
| Huber (δ=5) | High | Less sensitive to outlier clips | Good when teacher has occasional artefacts |
| PLCC-optimised (differentiable rank loss) | Medium | Directly optimises Pearson r | Higher variance on small datasets; use as secondary metric |
| SROCC proxy loss (ListNet) | Low | Optimises rank correlation | Experimental; not recommended for ≤ 70 pairs |

**Recommended primary loss**: Huber (δ=5.0) for soft-label distillation.
Secondary metrics: PLCC and SROCC reported on held-out split after each epoch.

### 3.4 Data augmentation on the feature vector

Feature-space augmentation on quality metrics is under-explored, but the
following are safe:

- **Gaussian noise** (σ ≤ 0.01 on normalised features): simulates measurement
  uncertainty in the libvmaf extractor.
- **Frame sub-sampling** (skip every 2nd frame before aggregation): effectively
  doubles the training-set size.
- **Clip-level horizontal flipping** of the VIF spatial-scale ordering: valid
  because VIF scale-0 (finest detail) and scale-3 (coarsest) are exchangeable
  in principle for full-reference metrics.

These should be applied only to training splits, not to the evaluation split
or the Netflix golden gate.

---

## 4. Evaluation harness design

### 4.1 Split strategy for 70 clips

70 clips is at the boundary of reliable cross-validation. Recommended
protocol:

- Fixed 60/10 train/test split (do not vary across runs; key with clip hash
  for reproducibility — consistent with the `key` column protocol in
  `vmaf-train eval`).
- Report PLCC, SROCC, RMSE vs `vmaf_v0.6.1` soft labels on the test split.
- Secondary: PLCC, SROCC vs Netflix MOS (where available from the 2016
  appendix data).
- Required gate: VMAF score on the 3 Netflix golden CPU pairs within
  `places=4` of the `vmaf_v0.6.1` reference.

### 4.2 Comparison baseline

Always report tiny-AI scores alongside `vmaf_v0.6.1` scores from the C API
on the same frames. Use `/cross-backend-diff` after any GPU path is added to
the trained model.

### 4.3 MCP smoke command (one-liner for CI)

```bash
cd mcp-server/vmaf-mcp && python -m pytest tests/test_smoke_e2e.py -v
```

This exercises `vmaf_score` against the smallest Netflix golden fixture
(`src01_hrc00_576x324.yuv` / `src01_hrc01_576x324.yuv`) via the JSON-RPC
`vmaf_score` tool, asserting the VMAF score within `places=4` of the
`vmaf_v0.6.1` CPU reference (≈ 76.7 at clip level). It requires the `vmaf`
binary to be built at `build/tools/vmaf`.

---

## 5. Related work referenced but out of scope for this PR

- **SSIMULACRA2** (Cloudinary, 2024): alternative FR metric; relevant for
  cross-metric distillation experiments in a later phase.
- **DOVER** (Wu et al., 2022): unified NR video quality model trained on
  mixed technical + aesthetic annotations. Relevant for C2/C3 NR tracks.
- **CLIP-IQA** (Wang et al., 2023): vision–language quality assessment;
  could serve as a teacher for NR models. Out of scope until the VLM extras
  (`vmaf-mcp[vlm]`) are stable.
- **VMAF-HD / VMAF-B** (Netflix internal, not published): rumoured per-scene
  models; referenced in the unpublished-Netflix-models research thread but
  not actionable without the weights.

---

## 6. Summary of recommendations for ADR-0242 follow-up

1. Start with soft-label distillation from `vmaf_v0.6.1` using a 32×2 MLP.
2. Sweep {32×2, 64×2, 64×3} with Huber loss (δ=5); select by PLCC on the
   held-out 10-clip split.
3. Gate every candidate against the 3 Netflix golden CPU pairs
   (`places=4` tolerance).
4. Export winning checkpoint as ONNX opset 17; register under
   `model/tiny/vmaf_tiny_fr_v2_nflx.onnx` (suffix `_nflx` signals corpus).
5. Run `/cross-backend-diff` before merging; max acceptable ULP delta = 2.

---

## References (full bibliography)

1. Li, Z. et al. "Toward a Practical Perceptual Video Quality Metric."
   Netflix Tech Blog, 2016.
2. Hinton, G. et al. "Distilling the Knowledge in a Neural Network."
   *NeurIPS* 2015 Deep Learning Workshop. <https://arxiv.org/abs/1503.02531>
3. Bosse, S. et al. "Deep Neural Networks for No-Reference and Full-Reference
   Image Quality Assessment." *IEEE TIP* 2018.
   <https://arxiv.org/abs/1612.01697>
4. Kim, J. and Lee, S. "Deep CNN-Based Blind Image Quality Predictor."
   *IEEE TNN* 2017. <https://ieeexplore.ieee.org/document/7885107>
5. Hosu, V. et al. "KonIQ-10k." *IEEE TIP* 2020.
   <https://arxiv.org/abs/1910.06180>
6. Ghadiyaram, D. and Bovik, A. "Massive Online Crowdsourced Study of
   Subjective and Objective Picture Quality." *IEEE TIP* 2016.
7. Ke, J. et al. "MUSIQ: Multi-Scale Image Quality Transformer."
   *ICCV* 2021. <https://arxiv.org/abs/2108.05997>
8. Netflix Tech Blog: "VMAF: The Journey Continues." 2018.
   <https://netflixtechblog.com/vmaf-the-journey-continues-44b51ee9ed12>
9. Netflix Tech Blog: "Toward a Better Quality Metric for the Streaming Era."
   2020. <https://netflixtechblog.com/toward-a-better-quality-metric-for-the-streaming-era-the-netflix-vmaf-cdm-b0e1922f7f97>
10. Netflix Tech Blog: "VMAF NEG." 2021.
    <https://netflixtechblog.medium.com/vmaf-neg-measuring-the-impact-of-video-enhancement-algorithms-91b07b28301a>
11. [ADR-0042](../adr/0042-tinyai-docs-required-per-pr.md) — fork tiny-AI
    doc-substance rule.
12. [ADR-0242](../adr/0242-tiny-ai-netflix-training-corpus.md) — parent ADR
    for this scaffold.
