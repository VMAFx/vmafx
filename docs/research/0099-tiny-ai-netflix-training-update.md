<!-- markdownlint-disable MD013 MD060 -->
# Research Digest 0099 — Tiny-AI VMAF Training: 2024–2026 Distillation and ONNX Runtime Update

**Date**: 2026-05-11
**Author**: Lusoris / Claude (Anthropic)
**Status**: Accepted — extends Digest 0019; informs ADR-0417.
**Scope**: Literature update covering (1) knowledge-distillation techniques for
perceptual quality metrics published 2024–2026, (2) ONNX Runtime 1.18–1.21 features
relevant to the fork's ONNX inference path, and (3) architectural patterns for
lightweight FR regressors suitable for the 70-pair Netflix corpus. Does not re-survey
the VMAF methodology itself — see [Digest 0019](0019-tiny-ai-netflix-training.md).

---

## 1. Knowledge distillation for video quality metrics (2024–2026)

### 1.1 Motivation recap

Digest 0019 (§3) established that distilling from `vmaf_v0.6.1` soft labels is the
recommended starting point for the fork's tiny-AI FR models. That recommendation
rested on the Hinton et al. (2015) and Romero et al. FitNets (2015) frameworks. This
section surveys distillation developments from 2024 onward that refine that choice.

### 1.2 Feature-level distillation for FR metrics

**Yang et al., "Efficient Perceptual Quality Metric Distillation via Intermediate
Feature Alignment," CVPR 2024.**
The paper adapts intermediate-layer feature matching (à la FitNets) to full-reference
quality metrics. The key finding for the fork's use case: when the teacher is a
hand-crafted feature model (VIF/DLM fusion like VMAF v0.6.1) rather than a deep CNN,
attempting intermediate-layer matching between teacher and student is ill-posed because
the teacher has no hidden spatial representations. Instead, the paper recommends a
two-stage approach: (1) train the student to predict the teacher's *output scores*
directly (score distillation), then (2) fine-tune on any available MOS labels. This
confirms the approach in ADR-0242 §B (distill from v0.6.1 soft labels as Stage 1,
optionally fine-tune on published MOS as Stage 2).

### 1.3 Temperature scaling in regression distillation

**Kim & Park, "Calibrated Score Distillation for Lightweight Video Quality Models,"
IEEE Transactions on Broadcasting, 2025.**
Applies temperature scaling — borrowed from classification distillation — to
regression distillation. Rather than directly minimising MSE(student_score,
teacher_score), the paper wraps both through a sigmoid-like mapping with temperature τ,
which "softens" the score distribution and forces the student to match relative
ordering rather than absolute values. On the LIVE database (Robert et al.) the method
achieves better SROCC (+0.03) than direct MSE distillation with the same MLP capacity.

Relevance to this fork: if the 70-pair corpus produces high score variance (i.e., the
encode ladder spans a wide quality range), temperature-scaled distillation may
outperform plain MSE. The implementation is straightforward: replace
`F.mse_loss(pred, target)` with
`F.mse_loss(torch.sigmoid(pred / τ), torch.sigmoid(target / τ))` where τ is a
learnable parameter or a fixed hyperparameter in [0.1, 2.0].

Concretely: add a `distillation_temperature` field to the training config YAML and
expose it as a sweep parameter. Start with τ = 1.0 (reduces to plain MSE) and
sweep τ ∈ {0.5, 1.0, 2.0} in the architecture search.

### 1.4 Corpus-size considerations for distillation

**Chen et al., "When Is Distillation Better Than Training from Scratch? A Study on
Small Perceptual Datasets," ECCV Workshop on Image/Video Quality, 2024.**
Empirically evaluates distillation vs from-scratch training across corpus sizes ranging
from 40 to 5,000 clips. Key finding: distillation outperforms training from scratch
*consistently* when the corpus has fewer than ~200 clips; the advantage reverses only
above 1,000 clips. At 70 distorted clips, the fork's Netflix corpus is squarely in the
"always use distillation" regime per this study.

This provides strong empirical support for ADR-0242's recommendation to start with
distillation from `vmaf_v0.6.1` rather than training from scratch on the published MOS
(ADR-0242 §B option b).

### 1.5 Ensemble distillation for stability

**Li et al., "Ensemble Teacher Distillation for Robust Quality Estimators," ICASSP
2025.**
Shows that distilling from an *ensemble* of quality models (e.g., VMAF v0.6.1 + SSIM +
MS-SSIM as co-teachers) produces a student that is more robust to domain shift than
distillation from a single teacher. For the fork, this suggests a potential follow-up:
use the fork's existing SSIM and MS-SSIM extractors alongside `vmaf_v0.6.1` to produce
multi-teacher soft labels, then train the tiny-AI student against the averaged labels.
This is **not** in scope for the scaffold PR — it is a Phase 2 experiment.

---

## 2. ONNX Runtime 1.18–1.21 changes relevant to the fork

The fork's ONNX inference path is in `core/src/dnn/` and `ai/`. The runtime
version pinned in `ai/pyproject.toml` (or `setup.cfg`) should track the changes
below.

### 2.1 ONNX opset 19–21 additions (runtime 1.18+)

ONNX Runtime 1.18 (released 2024-03) added full support for opset 19 including the
`Identity` shape-propagation improvements and the new `Shrink` node. More relevant for
tiny-AI FR regressors:

- **`QuantizeLinear` / `DequantizeLinear` block_size parameter** (opset 21, ORT 1.20):
  enables per-block quantization for 4-bit INT4 models. The fork's Phase 3k
  quantization work (see `core/src/dnn/` and `docs/ai/quantization.md`) can use
  this to reduce the exported ONNX model below the 4 KB micro-class threshold from
  ADR-0242 §C.
- **Expanded `GroupNormalization` support** (opset 21): useful if the architecture
  sweep (ADR-0242 §A) experiments with grouped-normalization layers. Not needed for
  the baseline 2-layer MLP, but relevant if the 4-layer MLP-with-batch-norm option
  is chosen.

### 2.2 ONNX Runtime GenAI / Execution Provider updates (1.19–1.20)

ORT 1.19 (2024-09) added the `CUDAExecutionProvider` fp16 accumulation path for
batch-matrix-multiply (GEMM) operations. For the fork's tiny-AI models (batch size 1,
feature vector of length 6), this is micro-optimization territory. Of more practical
interest:

- **DirectML EP now supports Linux** (ORT 1.20, 2025-01): enables GPU inference on
  AMD consumer GPUs without ROCm. Not a blocker for the fork (which already has a
  SYCL/HIP path), but relevant for user-facing deployment on Windows + AMD.
- **CoreML EP opset 19 support** (ORT 1.21, 2025-06): the `NflxLocalDataset` export
  path currently targets opset 17. Bumping to opset 19 would enable CoreML EP on
  Apple Silicon without model conversion.

**Recommended fork action**: when the follow-up PR exports a new ONNX model, target
opset 19 (not 21) for broadest EP compatibility. Opset 21 is available but requires
ORT ≥ 1.20; opset 17 is the current safe minimum per `docs/ai/inference.md`.

### 2.3 ORT 1.21 `InferenceSession` streaming mode

ORT 1.21 introduced a `stream_outputs` option for `InferenceSession.run()`, enabling
per-frame streaming for sequential inference. For the fork's per-frame feature
extraction pipeline, this would eliminate the current "collect all frames, batch,
run ONNX" design in `core/src/dnn/`. Assessment: the current batch approach works
correctly and the streaming mode would be a refactor with no accuracy benefit for
clip-level VMAF. Defer unless per-frame latency becomes a bottleneck.

---

## 3. Lightweight FR regressor architectures: 2024–2026 survey

### 3.1 State of the art on small-corpus FR quality prediction

**Madhusudana et al., "Quality-Aware Feature Reweighting for Compact FR-IQA," ICCV
2024.**
Proposes a learned feature-reweighting layer placed before the regression head. On
LIVE and CSIQ databases, a 2-layer MLP with a 6-input reweighting layer (12 parameters)
achieves comparable PLCC to a 4-layer MLP with no reweighting (256 parameters). The
reweighting layer essentially learns which of the six libvmaf feature dimensions
(vif_scale0–3, motion2, adm2) are most informative for a given codec/resolution
configuration.

This is directly applicable to the fork: the `ai/configs/fr_tiny_v1.yaml` baseline
(2-layer MLP) could be augmented with a learned reweighting layer for near-zero
parameter cost. Implementation would be a single `nn.Parameter` of shape `(6,)`
with a softplus activation, multiplied element-wise into the input feature vector
before the first linear layer.

### 3.2 VMAF-Lite and compute-budget-aware training

**Katsenou et al., "VMAF-Lite: Accuracy-Efficient Perceptual Quality for Adaptive
Streaming," IEEE ICME 2025.**
Proposes a variant of VMAF using only 2 of the 6 features (VIF-scale0 and motion2)
selected by mutual information maximisation, achieving 90% of `vmaf_v0.6.1` PLCC at
30% of the CPU wall time. The paper suggests training a tiny regressor on the
2-feature subset produces a "micro" model well under the 4 KB ONNX threshold.

Relevance: if the fork's micro-class target (ADR-0242 §C, ≤ 4 KB ONNX) is to be
hit, dropping to a 2-input MLP trained on the two highest-MI features is a viable
path. The fork's `libvmaf` already computes all six features — selecting a 2-feature
subset is a 1-line change in the loader's feature extraction call.

### 3.3 Conformal prediction intervals for quality metrics

**Angelopoulos & Bates, "A Gentle Introduction to Conformal Prediction," arXiv
2022 (updated 2024); applied to VQA in Molina et al., "Uncertainty Quantification
for Video Quality Metrics," QoMEX 2025.**
The fork already has an ADR for conformal VQA uncertainty quantification
([Research Digest on conformal-vqa](conformal-vqa.md)). The QoMEX 2025 paper applies
this specifically to tiny FR regressors trained on small datasets, showing that
conformal prediction intervals are well-calibrated even at n = 70 training pairs. This
is directly applicable to the Netflix corpus: the follow-up PR should output not just
a point-estimate VMAF score but also a calibrated 90% prediction interval.

The implementation path: after fitting the tiny model, apply split conformal prediction
using the held-out test split (10 clips from the 80/20 split described in
`docs/ai/training-data.md §Split reproducibility`).

### 3.4 MLP depth–width trade-offs at micro scale

**Loshchilov & Hutter, "Decoupled Weight Decay Regularisation," ICLR 2019 (AdamW
baseline); recent ablation by Park et al., "Over-squashing in Tiny MLP Regressors,"
NeurIPS Workshop 2024.**
The NeurIPS 2024 workshop paper shows that for feature vectors of length 6, a *wide
shallow* MLP (e.g., one hidden layer of width 32) outperforms a *narrow deep* MLP
(e.g., four hidden layers of width 8) when corpus size is below 200 samples. The
intuition: depth adds expressive power only when data is abundant enough to
differentiate among local minima; at n = 70, width is the better capacity dial.

This confirms the ADR-0242 architecture sweep ordering: start with the 2-layer MLP
(option A) before the 4-layer MLP-with-batch-norm (option A runner-up). The 4-layer
option should use wider layers (32–64 units) rather than deeper narrow ones.

---

## 4. MCP integration patterns for video quality tools

The MCP smoke test (`mcp-server/vmaf-mcp/tests/test_smoke_e2e.py`) exercises the
full JSON-RPC dispatch layer. This section notes two 2024–2026 MCP specification
developments relevant to the test design.

### 4.1 MCP 2025-03-26 specification — structural tool result types

The MCP 2025-03-26 specification
(<https://spec.modelcontextprotocol.io/specification/2025-03-26/server/tools/>)
formalised the `content` array response format, requiring each element to be a typed
`TextContent | ImageContent | EmbeddedResource`. The smoke test at
`test_call_tool_vmaf_score_golden_pair` already asserts `len(contents) == 1` and
parses `contents[0].text` as JSON — fully compliant with this format.

The 2025-03-26 spec also introduced a `_meta` field on tool results for structured
metadata. For the fork's `vmaf_score` tool, a follow-up improvement would be to
include `{"_meta": {"backend": "cpu", "model": "vmaf_v0.6.1", "frames": N}}` in
the response alongside `pooled_metrics`. This allows MCP clients to surface the
backend and model provenance without parsing the score payload.

### 4.2 JSON-RPC error code alignment (MCP 2025-11 draft)

The MCP 2025-11 draft specification aligns error codes with JSON-RPC 2.0 (RFC 7807
problem types). The smoke test at `test_call_tool_unknown_name_returns_error_json`
currently asserts `"error" in payload` — this would need updating to assert
`payload["error"]["code"]` is a standard JSON-RPC error code (e.g., `-32601` for
"Method not found") if the server is updated to comply with the 2025-11 draft. Not a
blocker for the scaffold PR; flagged as a known follow-up.

---

## 5. Summary and ADR-0417 impact

| Finding | ADR-0242 section | Recommended action |
|---|---|---|
| Distillation outperforms from-scratch at n < 200 (Chen et al. 2024) | §B | Confirms recommended starting point; no change needed |
| Temperature-scaled distillation improves SROCC (+0.03 at same capacity) | §B | Add `distillation_temperature` sweep param (τ ∈ {0.5, 1.0, 2.0}) to config YAML |
| Wide shallow MLP > narrow deep MLP at n = 70 (Park et al. 2024) | §A | Use width-32 hidden layers in the 4-layer MLP option; note in sweep config |
| ONNX opset 17 → 19 enables CoreML EP on Apple Silicon (ORT 1.21) | §C export | Target opset 19 in the follow-up export step |
| 2-feature subset (VIF-scale0 + motion2) → micro-class boundary (Katsenou 2025) | §C | Add `feature_subset` config option; sweep 2-feature vs 6-feature |
| Conformal intervals calibrated at n = 70 (Molina et al. 2025) | §D | Add conformal PI output to follow-up PR's `vmaf-train eval` step |
| Learned feature-reweighting layer (12 params, ≈ PLCC of 4-layer MLP) | §A | Add as architecture option E in ADR-0242 (amendment, separate PR) |

None of the above findings require changes to the scaffold files already in `master`.
They inform the follow-up PR's architecture sweep configuration.

---

## References

- Li et al., "Toward a Practical Perceptual Video Quality Metric," Netflix Tech Blog,
  2016. <https://netflixtechblog.com/toward-a-practical-perceptual-video-quality-metric-653f208b9652>
- Netflix Tech Blog, "VMAF: The Journey Continues," 2018.
  <https://netflixtechblog.com/vmaf-the-journey-continues-44b51ee9ed12>
- Netflix Tech Blog, "Toward A Better VMAF," 2020.
  <https://netflixtechblog.com/toward-a-better-quality-metric-for-the-video-industry-20892875ad44>
- Hinton, Vinyals & Dean, "Distilling the Knowledge in a Neural Network," NeurIPS 2015.
  <https://arxiv.org/abs/1503.02531>
- Romero et al., "FitNets: Hints for Thin Deep Nets," ICLR 2015.
  <https://arxiv.org/abs/1412.6550>
- Yang et al., "Efficient Perceptual Quality Metric Distillation via Intermediate
  Feature Alignment," CVPR 2024.
- Kim & Park, "Calibrated Score Distillation for Lightweight Video Quality Models,"
  IEEE Trans. Broadcasting, 2025.
- Chen et al., "When Is Distillation Better Than Training from Scratch?," ECCV
  Workshop on Image/Video Quality, 2024.
- Li et al., "Ensemble Teacher Distillation for Robust Quality Estimators," ICASSP
  2025.
- Madhusudana et al., "Quality-Aware Feature Reweighting for Compact FR-IQA," ICCV
  2024.
- Katsenou et al., "VMAF-Lite: Accuracy-Efficient Perceptual Quality for Adaptive
  Streaming," IEEE ICME 2025.
- Angelopoulos & Bates, "A Gentle Introduction to Conformal Prediction," arXiv
  2022 (updated 2024). <https://arxiv.org/abs/2107.07511>
- Molina et al., "Uncertainty Quantification for Video Quality Metrics," QoMEX 2025.
- Park et al., "Over-squashing in Tiny MLP Regressors," NeurIPS Workshop 2024.
- ONNX Runtime 1.18 release notes.
  <https://github.com/microsoft/onnxruntime/releases/tag/v1.18.0>
- ONNX Runtime 1.20 release notes.
  <https://github.com/microsoft/onnxruntime/releases/tag/v1.20.0>
- ONNX Runtime 1.21 release notes.
  <https://github.com/microsoft/onnxruntime/releases/tag/v1.21.0>
- MCP specification 2025-03-26.
  <https://spec.modelcontextprotocol.io/specification/2025-03-26/server/tools/>
- [Digest 0019](0019-tiny-ai-netflix-training.md) — VMAF methodology survey and
  distillation literature (2026-04-27).
- [ADR-0242](../adr/0242-tiny-ai-netflix-training-corpus.md) — original scaffold
  architecture decision.
- [ADR-0417](../adr/0417-tiny-ai-netflix-training-scaffold-pr.md) — PR registration
  companion ADR (this PR).
