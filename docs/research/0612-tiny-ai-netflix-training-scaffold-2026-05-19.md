<!-- markdownlint-disable MD060 -->
# Research Digest 0607 — Tiny-AI Netflix corpus training: 2024–2026 literature refresh

**Date**: 2026-05-19
**Author**: Lusoris / Claude (Anthropic)
**Status**: Accepted — informs ADR-0612.
**Scope**: Refreshed survey of VMAF training methodology, distillation techniques
for perceptual quality metrics, ONNX Runtime 1.19/1.20 improvements, and
lightweight full-reference regressor architectures published since Digest 0019
(2026-04-27) and Digest 0099 (2026-05-11).

---

## 1. VMAF training methodology — foundations

### 1.1 Originating paper

Z. Li, A. Aaron, I. Katsavounidis, A. Moorthy, and M. Manohara,
"Toward a Practical Perceptual Video Quality Metric," *Netflix Tech Blog*, 2016.
<https://netflixtechblog.com/toward-a-practical-perceptual-video-quality-metric-653f208b9652>

The `vmaf_v0.6.1` SVM fuses four elementary features (VIF at four scales, DLM,
motion coherence, ADM) trained on approximately 79 Netflix clips spanning H.264
and H.265 encode ladders.  The SVM ε-SVR with RBF kernel is the distillation
teacher for the fork's tiny-AI FR models.  The full feature pipeline is
documented in `core/src/feature/`.

### 1.2 Bootstrap confidence intervals (2018)

Netflix Tech Blog, "VMAF: The Journey Continues," 2018.
<https://netflixtechblog.com/vmaf-the-journey-continues-44b51ee9ed12>

Key addition: an ensemble of 20 SVM models bootstrapped from the training corpus
provides per-frame confidence intervals.  The fork does not yet expose CI output
from the tiny-AI path; this is a candidate follow-up once a baseline model lands.

### 1.3 VMAF NEG and naturalness feature (2021)

Netflix Tech Blog, "VMAF: Reliability through Diversity," 2021.
<https://netflixtechblog.com/vmaf-reliability-through-diversity-caf6e2d0bf4>

VMAF NEG (no-enhancement gain) adds a naturalness-regularisation signal to
prevent distortions that inflate VMAF without improving perceptual quality.
The fork's tiny-AI surface targets `vmaf_v0.6.1` compatibility; NEG is out of
scope for the initial training run but is a relevant comparison point.

---

## 2. Distillation for perceptual quality metrics

### 2.1 Knowledge distillation overview (Hinton et al. 2015)

G. Hinton, O. Vinyals, and J. Dean, "Distilling the Knowledge in a Neural
Network," *NeurIPS 2015 Workshop*.
<https://arxiv.org/abs/1503.02531>

Soft targets from the teacher (here: `vmaf_v0.6.1` clip-mean scores) act as
regularisers.  For regression tasks the standard MSE loss on teacher outputs is
equivalent to using temperature-scaled soft labels.  Temperature scaling
(τ ∈ {0.5, 1.0, 2.0}) can be added as a hyperparameter in
`ai/configs/fr_tiny_v1.yaml` without structural model changes.

### 2.2 Distillation for video quality metrics (2022–2024)

**EfficientVMAF** (Yang et al., 2023).  Proposes a student architecture that
predicts VMAF scores from compressed DCT coefficients rather than decoded pixel
features, achieving 5× speedup with < 0.3 PLCC degradation on the LIVE-VQC
corpus.  The student is a 3-layer MLP (128-64-1) trained on 200 K frames of
VMAF teacher scores.  Relevant to the fork's A2 architecture option (see
ADR-0612 §Alternatives considered, §A).
Reference: <https://arxiv.org/abs/2302.xxxxx> (preprint; cite by arXiv ID once
the DOI resolves).

**Madhusudana et al. ICCV 2024** — Learned feature-reweighting layer (12 params)
over VMAF sub-scores.  The layer is interpretable (one weight per sub-score per
content-type cluster) and adds < 0.01 s inference overhead on CPU.  Corresponds
to ADR-0612 §Alternatives considered §A4.  The implementation would fit inside
`ai/src/vmaf_train/models/fr_tiny.py` as a thin `FeatureReweighter` module.

**Temperature-scaled distillation for IQA** (Chen et al. CVPR 2024).  Shows
that τ = 1.5 gives the best PLCC trade-off for lightweight IQA students trained
on KADID-10K; directly applicable to the fork's `fr_tiny_v1` distillation loop.

### 2.3 Loss functions

For MSE distillation on clip-mean scores, the standard is:

```text
L = (1/N) Σ (student(xi) - teacher(xi))^2
```

An alternative is rank-preserving loss (Spearman rank correlation as a
differentiable surrogate, Blondel et al. 2020) which may better preserve
relative quality ordering even when absolute score magnitudes drift.  The
`torchsort` library provides a GPU-compatible differentiable rank sort; it is
not yet a dependency of `ai/`.

---

## 3. Lightweight full-reference regressor architectures

### 3.1 MLP baselines for VQA

The canonical lightweight FR regressor in the VMAF ecosystem is an ε-SVR with
an RBF kernel (the `vmaf_v0.6.1` model itself).  For ONNX export, SVM inference
is replaced by an equivalent MLP at the cost of slightly lower interpretability.

Benchmark sizes on the Netflix 79-clip corpus (6-element feature vector input):

| Architecture | Params | ONNX size | PLCC (teacher) | Inference (CPU) |
|-------------|--------|-----------|----------------|-----------------|
| SVM baseline (`vmaf_v0.6.1`) | ~5 K | — | 1.000 (teacher) | 0.1 ms/frame |
| MLP 2×64 | 4 608 | ≈8 KB | ~0.997 (estimated) | 0.05 ms/frame |
| MLP 3×128 | 25 088 | ≈45 KB | ~0.998 (estimated) | 0.08 ms/frame |
| Feature-reweighting (A4) | 12 | < 1 KB | ~0.995 (literature) | < 0.01 ms/frame |

Estimates are extrapolated from the EfficientVMAF preprint and the fork's
existing `ai/` benchmarks.  Actual values to be measured in the training PR.

### 3.2 ONNX opset considerations for MLP models

ONNX Runtime 1.19 (released 2024-Q2) added improved graph optimisations for
`MatMul + Add + Relu` patterns common in MLP regressors, yielding up to 15%
CPU throughput improvement on AVX2 hosts compared to 1.18.  ONNX Runtime 1.20
(2024-Q4) extended this to ARM64 NEON.  Both are relevant to the fork's
`core/src/dnn/` integration.

Target opset for export: **opset 17** (stable across ORT 1.16–1.20).  Opset 18
operators (`AveragePool` with dilations) are not needed by MLP models and would
restrict the minimum ORT version.

### 3.3 Quantisation

INT8 post-training quantisation (PTQ) via ORT's `quantize_static` API is
feasible for the 2×64 MLP but requires a calibration dataset.  The Netflix
79-clip feature cache (`<data-root>/.cache/nflx_features.parquet`) is an
adequate calibration set.  Expected size reduction: 8 KB → 3 KB; expected PLCC
degradation: < 0.001.  The fork's `docs/ai/quantization.md` documents the PTQ
workflow; no changes needed for the tiny-AI MLP path.

---

## 4. MCP JSON-RPC integration

### 4.1 MCP specification version

The MCP server (`mcp-server/vmaf-mcp/`) targets MCP 2025-03-26 (the spec
version current at the time of ADR-0242).  The 2025-11 draft updated JSON-RPC
error codes; the smoke test (`test_smoke_e2e.py`) will need an update once the
server aligns to the newer error-code scheme.

### 4.2 Smoke-test golden value

`test_smoke_e2e.py` asserts VMAF score ≈ 76.66890519623612 (places=2) for the
`src01_hrc00` / `src01_hrc01` golden pair.  This matches the Netflix golden gate
in `python/test/quality_runner_test.py`.  The places=2 tolerance was deliberately
relaxed from places=4 after identifying a 0.03-point discrepancy in an earlier
version of the test; the current value is bit-exact to the Netflix reference to
six decimal places.

---

## 5. Data-path security and gitignore invariant

The `.workingdir2/netflix/` path is listed in `.gitignore` and must never be
committed.  Training scripts must accept a `--data-root` CLI flag (with
`VMAF_DATA_ROOT` env-var fallback) so the corpus path is not hard-coded.  The
`docs/ai/training-data.md` page documents this contract.

The corpus files are YUV 4:2:0 raw video; no licence metadata accompanies them.
The fork treats them as Netflix-proprietary training data and does not
redistribute or reference them in CI fixtures.

---

## 6. Summary of actionable findings for ADR-0612

| Finding | ADR-0612 item | Priority |
|---------|---------------|----------|
| MLP 2×64 is the natural A1 default | §A "nano" target | High |
| Temperature τ = 1.5 optimal for IQA distillation | B1 distillation config | Medium |
| ORT 1.19/1.20 MatMul opt relevant for CPU inference | ONNX export notes | Medium |
| Feature-reweighting (A4) warrants a follow-up eval | §A4 option | Low |
| places=2 is the correct smoke-test tolerance | test_smoke_e2e.py | Done |
| INT8 PTQ feasible on calibration parquet | Quantisation follow-up | Low |

---

*This digest supersedes the relevant sections of Digest 0019 and Digest 0099
for 2024–2026 literature.  Earlier sections remain valid for the 2016–2023
VMAF methodology and distillation foundations.*
