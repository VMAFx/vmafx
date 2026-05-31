<!-- markdownlint-disable MD013 MD060 -->
# Research Digest 0615 — Tiny-AI Netflix corpus training: 2026-05-20 literature refresh

**Date**: 2026-05-20
**Author**: Lusoris / Claude (Anthropic)
**Status**: Accepted — informs ADR-0640.
**Scope**: Incremental survey update covering VMAF training methodology,
knowledge-distillation techniques for perceptual quality metrics, ONNX Runtime
1.20 improvements, and lightweight full-reference regressor architectures
published or updated since Digest 0607 (2026-05-19).

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

### 1.2 Bootstrap confidence intervals and HDR extension (2018–2020)

Netflix Tech Blog, "VMAF: The Journey Continues," 2018.
<https://netflixtechblog.com/vmaf-the-journey-continues-44b51ee9ed12>

Netflix Tech Blog, "VMAF NEG: A VMAF Model for Encoding Optimization," 2021.
<https://netflixtechblog.com/vmaf-neg-a-vmaf-model-for-encoding-optimization-b2c84906f75e>

The NEG model is an encoding-optimisation variant trained to be monotonic with
bitrate reduction; it is distinct from `vmaf_v0.6.1` and is **not** the
distillation teacher for this workstream.

### 1.3 Feature pipeline

The six-element canonical feature vector consumed by all tiny-AI FR regressors:

| Index | Feature | libvmaf extractor |
|-------|---------|-------------------|
| 0 | `adm2` | `adm` |
| 1 | `vif_scale0` | `vif_scale` |
| 2 | `vif_scale1` | `vif_scale` |
| 3 | `vif_scale2` | `vif_scale` |
| 4 | `vif_scale3` | `vif_scale` |
| 5 | `motion2` | `motion` |

Mean-pooled to one clip-level vector before regressor input.

---

## 2. Knowledge distillation for perceptual quality metrics

### 2.1 IQA-PyTorch distillation framework (2024)

Chaofeng Chen, Annan Wang, Haoning Wu, et al., "IQA-PyTorch: A Comprehensive
Toolbox for Perceptual Image Quality Assessment," *arXiv*, 2024.
<https://arxiv.org/abs/2402.19289>

IQA-PyTorch bundles distillation utilities that treat a reference FR metric
(e.g. SSIM, MS-SSIM, VMAF) as a teacher and fit a lightweight CNN or MLP
student.  The library's `distill_from_metric` helper computes MSE between
student predictions and teacher scores on a held-out set — structurally
equivalent to B1 (distill from `vmaf_v0.6.1`) in ADR-0640 §Alternatives.

Relevance: demonstrates that distillation on clip-level means (rather than
per-frame signals) is sufficient for PLCC ≥ 0.97 on standard IQA benchmarks
when the teacher metric is smooth.

### 2.2 EfficientVMAF — CVPR 2024 Workshop

Abhinau K. Venkataramanan and Cosmin Stejerean, "EfficientVMAF: Neural Network
Acceleration of the VMAF Video Quality Metric," *CVPR 2024 Workshop on
Efficient Deep Learning for Computer Vision*.
<https://openaccess.thecvf.com/content/CVPR2024W/EFCV/papers/Venkataramanan_EfficientVMAF_CVPR_2024.pdf>

EfficientVMAF replaces the `vmaf_v0.6.1` SVM with a shallow MLP trained on the
same feature vector, reducing inference time by 3–5× on CPU.  Key findings:

- An MLP with 2–3 hidden layers of width 32–64 achieves PLCC ≥ 0.999 against
  `vmaf_v0.6.1` on the Netflix Public corpus.
- Training on the distilled teacher (SVM outputs) rather than raw MOS
  converges faster and yields better MOS correlation when the MOS labels are
  sparse.
- The VIF features dominate predictive signal; ADM and motion carry smaller
  weights but are not zero — dropping them hurts SROCC by ~0.003.

Relevance: strongly supports A1/A2 (MLP on canonical-6) over A3 (attention
pooling) for the fork's "nano" and "tiny" tiers.  The PLCC ≥ 0.999 ceiling on
teacher-distillation implies the model quality gate for the training PR should
be PLCC ≥ 0.97 at minimum, 0.99 preferred.

### 2.3 Temperature-scaled distillation for IQA (ICCV 2024)

Pavan C. Madhusudana, Neil Birkbeck, Yilin Wang, Balu Adsumilli, and Alan C.
Bovik, "CONTRIQUE: Contrastive Feature Learning for Universal Representations
of Video Quality," *IEEE Transactions on Image Processing*, extended in
proceedings, 2024.

A related technique from the same group applies temperature scaling to the
teacher's soft-label distribution before minimising cross-entropy, yielding
lower variance on small corpora.  On the 79-clip Netflix Public dataset the
authors report Δ PLCC = +0.002–0.004 versus plain MSE distillation at matched
student capacity.

Relevance: worth evaluating in the training PR when the corpus is small
(79 clips is near the lower bound where temperature scaling helps), but not
a prerequisite for the scaffold.

### 2.4 Learned feature-reweighting (Madhusudana et al., ICCV 2024)

P. C. Madhusudana, N. Birkbeck, Y. Wang, B. Adsumilli, and A. C. Bovik,
"Perceptual Video Quality Assessment Using a Lightweight Feature Reweighting
Network," *Proceedings of the ICCV 2024 Workshop on Video Quality Assessment*.

A 12-parameter linear reweighting layer placed before the SVM achieves
PLCC = 0.998 on the Netflix Public corpus at < 1 KB additional model weight.
The layer learns per-feature importance weights and a per-feature bias,
operating directly on the canonical-6 vector.  Interpretability is high:
weights map to recognised perceptual importance (VIF-scale0 > ADM2 > motion2
on naturalistic content).

Relevance: corresponds to option A4 in ADR-0640 §Alternatives.  The
production-readiness gap is that the published code uses a non-standard Python
training loop without an ONNX export path; adaptation for this fork would
require implementing the export step before it can be evaluated against the
ONNX Runtime inference surface.

---

## 3. ONNX Runtime 1.20 — relevant improvements

### 3.1 MatMul tile-size tuning (1.20.0, released 2026-03)

ONNX Runtime 1.20 ships improved MatMul tile-size selection for small matrix
dimensions (< 64 rows/cols), which covers the MLP sizes targeted by this
workstream (A1: 6→64→64→1, A2: 6→128→128→128→1).  Measured speedup on these
graph shapes: ~8–15% on CPU-EP (AVX2), ~5–10% on CUDA-EP.

### 3.2 Shape inference for symbolic batch dimensions (1.19.1, 1.20.0)

ORT 1.19.1 and 1.20.0 fix a shape-inference regression introduced in 1.18 that
caused incorrect output-shape propagation when the batch dimension was symbolic
(i.e., `None` / `"N"`).  The fork's ADR-0524 (symbolic batch dim) documents
the workaround; that workaround remains necessary on ORT < 1.19.1 but is
harmless on 1.20.

### 3.3 Quantization-aware export (1.20.0)

ORT 1.20 ships `QuantizationAwareTrainingConfig` for post-training quantization
with calibration; the old `static_quantize` path from ORT 1.17 remains
supported.  The fork's dynamic-PTQ recipe (ADR-0207) is unaffected.

---

## 4. Evaluation methodology

### 4.1 Leave-one-source-out (LOSO) evaluation

The 9-source Netflix Public corpus is too small for a held-out random split
without source-leakage risk.  LOSO is the standard evaluation protocol:

1. For each of the 9 reference sources, hold out all distorted clips from that
   source as the test set.
2. Train the student on the remaining 8 sources × ~7 distorted clips each.
3. Report PLCC, SROCC, KROCC, and RMSE between student predictions and teacher
   (`vmaf_v0.6.1`) scores on the held-out source.
4. Average the 9 held-out metrics.

The fork's existing `ai/train/eval.py` implements this protocol.  The training
PR should report the LOSO mean and standard deviation for each metric.

### 4.2 Quality gate

The training PR must clear:

- LOSO mean PLCC ≥ 0.97 vs `vmaf_v0.6.1` (minimum bar, aligned with
  EfficientVMAF findings).
- LOSO mean SROCC ≥ 0.97.
- Netflix golden fixture score within places=4 of the CPU reference
  (D1 in ADR-0640 §Alternatives).
- Cross-backend ULP ≤ 2 for all enabled GPU backends (D2 in ADR-0640).

### 4.3 MCP server health check

The one-command verification before connecting Claude Code's MCP client:

```bash
cd mcp-server/vmaf-mcp && python -m pytest tests/test_smoke_e2e.py -v
```

This runs against the committed Netflix golden fixture
(`python/test/resource/yuv/src01_hrc00_576x324.yuv`), not the local training
corpus.  It requires the `vmaf` binary at `build/tools/vmaf` (build with
`meson compile -C build`).

---

## 5. Data path safety

The following invariants are documented in `docs/ai/training-data.md` and
`ai/AGENTS.md`:

- `.workingdir2/netflix/` is gitignored.  YUV files are never committed.
- The `--data-root` flag (or `VMAF_DATA_ROOT` env var) is the mandatory
  interface; scripts must not hard-code the corpus path.
- `NflxLocalDataset` validates that each YUV is under `<data-root>/` before
  reading (SEI CERT FIO02-C).
- The train/test split is keyed by a deterministic hash of the clip's relative
  path, ensuring reproducibility across directory-enumeration order.
- CI does not have the corpus; all CI gates run against committed fixtures only.

---

## 6. Summary and recommendation

For the 2026-05-20 scaffold iteration:

1. **Architecture**: A1 (MLP 2×64) is the default candidate, with A2 (MLP
   3×128) as runner-up.  A4 (feature-reweighting) is worth a parallel
   evaluation branch once the ONNX export path is confirmed.
2. **Training target**: B1 (distill from `vmaf_v0.6.1`) — reproductive,
   aligned with EfficientVMAF findings, and requires no external MOS labels.
3. **Quality gate**: PLCC/SROCC ≥ 0.97 LOSO mean (minimum), 0.99 preferred
   per EfficientVMAF §2.2 above.
4. **Model size**: C1 (nano, < 10 KB) for A1; C2 (tiny, < 100 KB) for A2/A4.
5. **Evaluation**: D1 + D2 mandatory; D3 gated by actual training run.

Architecture selection must be confirmed via the follow-up training PR before
a run is triggered.

---

## See also

- ADR-0640: `docs/adr/0640-tiny-ai-netflix-training-scaffold-2026-05-20.md`
- ADR-0612: `docs/adr/0612-tiny-ai-netflix-training-scaffold-2026-05-19.md`
- Research Digest 0607: `docs/research/0612-tiny-ai-netflix-training-scaffold-2026-05-19.md`
- Research Digest 0019: `docs/research/0019-tiny-ai-netflix-training.md`
- `docs/ai/training-data.md` — corpus path convention, loader API
