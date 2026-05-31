<!-- markdownlint-disable MD060 -->
# Research-0043 — DISTS extractor design digest

| Field      | Value                                                  |
| ---------- | ------------------------------------------------------ |
| **Date**   | 2026-05-01                                             |
| **Status** | Design only — implementation tracked as T7-DISTS        |
| **Tags**   | dnn, tiny-ai, fr, dists, lpips, onnx                   |

Companion to [ADR-0236](../adr/0236-dists-extractor.md). Captures the
*why* behind the design choices in that ADR plus the empirical and
implementation context that doesn't belong in the ADR's
decision-record body.

## What DISTS is

Ding, Ma, Wang, Simoncelli, *Image Quality Assessment: Unifying
Structure and Texture Similarity*, IEEE PAMI 2020.

Given a reference image *x* and distorted image *y*, both ImageNet-
normalised, the metric extracts VGG-16 features at five layers
(`relu1_2`, `relu2_2`, `relu3_3`, `relu4_3`, `relu5_3`). For each
channel of each feature map, two statistics are computed:

- **Texture term**: `(2 µ_x µ_y + c1) / (µ_x² + µ_y² + c1)`, the
  channel-wise mean similarity (the SSIM luminance term applied to
  feature means).
- **Structure term**: `(2 σ_xy + c2) / (σ_x² + σ_y² + c2)`, the
  channel-wise variance and cross-covariance similarity (the SSIM
  contrast / structure term applied to feature variances).

The two terms are combined with learned per-channel weights
(`α_i`, `β_i`) and summed over channels and layers. The final
score is a scalar in `[0, 1]` where 1 means identical perceptual
content. The published DISTS exposes this as a *distance* (1 - score),
following the LPIPS convention.

## Why this matters for the fork

The fork ships `lpips_sq` ([ADR-0041](../adr/0041-lpips-onnx-extractor.md))
as its only deep-feature FR extractor. Bristol VI-Lab's 2026 NVC
review (audited as
[Research-0033](0033-bristol-nvc-review-2026.md)) §5.3 lists DISTS
co-equally with LPIPS as the canonical deep-feature FR pair for
video quality. They're not redundant: LPIPS measures normalised
feature *distances*; DISTS measures *similarity of feature
statistics*. On synthetic-distortion benchmarks (KADID-10k, TID2013)
DISTS reports SROCC ≥ LPIPS by 0.02-0.05; on natural-distortion
benchmarks (LIVE-IQA) the two are comparable.

For the fork's tiny-AI surface, shipping DISTS alongside LPIPS:

1. Gives downstream consumers a second perceptual FR signal at
   essentially zero additional integration cost (same VGG-16
   forward pass shape; same input preprocessing).
2. Closes Research-0033's actionable #5.
3. Aligns the fork's deep-feature FR surface with the citations the
   Bristol audit shows are standard in NVC literature.

## Why not just one of them

Three considered postures:

- **Both** (chosen). Symmetric surface; lets users pick by
  workload. Marginal maintenance cost.
- **DISTS only**, retire LPIPS. Forces consumers onto the
  better-correlating metric. *Rejected* in ADR-0236 — backwards-
  incompatible and LPIPS has wider literature lineage.
- **Combined ONNX** that emits both scalars from one VGG pass. Saves
  half the per-frame cost. *Rejected* in ADR-0236 — conflates two
  distinct papers' metrics, complicates the model card / license
  trail, and the saving is small (VGG forward dominates either way).

## ABI shape (mirrors `lpips_sq`)

Inputs:

- `ref` — `[1, 3, H, W]` float32, ImageNet-normalised RGB.
- `dist` — `[1, 3, H, W]` float32, ImageNet-normalised RGB.

Output:

- `score` — `[]` (scalar) float32. Convention: emit the *distance*
  form `1 - similarity` so larger = worse, mirroring LPIPS. Document
  this clearly in the model card.

The `H` and `W` are dynamic (the existing LPIPS pipeline already
handles this via runtime input-shape determination in
[`core/src/dnn/`](../../core/src/dnn/)).

## Op-allowlist sanity check

DISTS-from-VGG-16 hits these ONNX ops:

| Op                 | Already in the LPIPS path? | Notes                                     |
| ------------------ | -------------------------- | ----------------------------------------- |
| `Conv` (4D)        | yes                        | All VGG conv layers                       |
| `Relu`             | yes                        |                                           |
| `MaxPool` (4D)     | yes                        | Inter-block pooling                       |
| `Flatten`          | yes                        |                                           |
| `Add`              | yes                        | SSIM-style sums                           |
| `Sub`              | yes                        |                                           |
| `Mul`              | yes                        |                                           |
| `Div`              | yes                        |                                           |
| `Pow` (2)          | yes                        | µ², σ² (channel statistics)               |
| `ReduceMean`       | yes                        | Channel-wise mean over H, W              |
| `Constant`         | yes                        | The `c1`, `c2` numerical-stability constants |

No new ops needed beyond what `lpips_sq` already requires per
[Research-0006](0006-tinyai-ptq-accuracy-targets.md) §2. The
op-allowlist gate in `core/src/dnn/op_allowlist.c` (or wherever it
lives) does not need to grow.

## PTQ posture

Per-channel weights `α_i`, `β_i` are small (5 layers × ~512 channels
on average = ~5 K weights) and known to be quantisation-tolerant —
DISTS's combination is monotone in its inputs, so per-channel
quantisation noise on the weights is dominated by the noise on the
per-channel feature statistics, which is what SSIM-style stability
constants `c1`, `c2` exist to absorb.

Static PTQ should clear a 0.005 PLCC budget on first try; if it
doesn't, fall back to dynamic PTQ as the LPIPS path does (no QAT
needed for v1). The harness lives at
[`ai/scripts/measure_quant_drop.py`](../../ai/scripts/measure_quant_drop.py)
and the budget convention is registered in `model/tiny/registry.json`
under each entry's `quant_accuracy_budget_plcc`.

## Smoke / placeholder strategy

Mirrors the FastDVDnet / MobileSal / TransNet V2 placeholder pattern:
ship a randomly-initialised tiny ONNX with the right I/O shape and
mark it `smoke: true`. The smoke ONNX fits the registry's
sha256-pinning contract and exercises the runtime loader without
requiring trained weights to land first.

Real upstream weights — Ding's reference at
[github.com/dingkeyan93/DISTS](https://github.com/dingkeyan93/DISTS),
MIT-licensed — track as **T7-DISTS-followup** in the backlog. That
follow-up does:

1. Pull the reference weights file.
2. Port the network arithmetic verbatim into a clean PyTorch model
   (the reference repo mixes inference + research notebooks; we
   want a clean export driver).
3. Export to ONNX opset ≥ 17 via `torch.onnx.export`, exclude the
   `softmax`/`mean-by-spatial` head if the published model wraps the
   raw scalar in extra ops (we want the raw scalar; user-side
   softening is the consumer's responsibility).
4. Verify per-frame parity against the reference repo's PyTorch eval
   on a 20-sample LIVE-IQA / TID2013 subset.
5. Run the sigstore-bundle pipeline ([ADR-0211](../adr/0211-model-registry-sigstore.md)).

## Performance expectation

VGG-16 forward at 1080p input on a recent x86 CPU is ~120-180ms per
frame in fp32 ONNX Runtime. With static int8 PTQ that drops to
~40-60ms. Per-frame DISTS adds the channel-stat math which is O(C×L)
— microseconds, negligible. Same order as LPIPS.

For real-time scoring, the realistic deployment is the
already-existing CUDA / Vulkan compute path the LPIPS extractor
uses — the VGG forward dominates and the GPU twins are documented
in [`docs/backends/`](../backends/).

## References

- Ding, Ma, Wang, Simoncelli, *Image Quality Assessment: Unifying
  Structure and Texture Similarity*, IEEE PAMI 2020.
  [doi:10.1109/TPAMI.2020.3045810](https://doi.org/10.1109/TPAMI.2020.3045810).
- Zhang, Isola, Efros, Shechtman, Wang, *The Unreasonable
  Effectiveness of Deep Features as a Perceptual Metric*, CVPR 2018.
- Gao et al., *Advances in Neural Video Compression: A Review and
  Benchmarking*, Bristol VI-Lab 2026.
- [`docs/research/0033-bristol-nvc-review-2026.md`](0033-bristol-nvc-review-2026.md)
  — actionable items table, item #5.
- [`docs/research/0006-tinyai-ptq-accuracy-targets.md`](0006-tinyai-ptq-accuracy-targets.md)
  — PTQ budget convention.
- [github.com/dingkeyan93/DISTS](https://github.com/dingkeyan93/DISTS) —
  Ding's MIT-licensed reference implementation; the upstream-weights
  source for T7-DISTS-followup.
