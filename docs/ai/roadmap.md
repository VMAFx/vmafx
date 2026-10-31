# Tiny-AI — roadmap

This page shows what is shipped, planned and deferred on the tiny-AI surface.
The four capabilities already in-tree are described in
[overview.md](overview.md). The roadmap covers the expansion beyond the scope of
[ADR-0020](../adr/0020-tinyai-four-capabilities.md) to
[ADR-0023](../adr/0023-tinyai-user-surfaces.md).

!!! note
    Wave 1 is locked by [ADR-0107](../adr/0107-tinyai-wave1-scope-expansion.md)
    (supersedes [ADR-0036](../adr/0036-tinyai-wave1-scope-expansion.md), the
    original 2026-04-17 popup approval). Later waves are non-binding and
    document direction only.

!!! note
    The shipped models were trained against the `vmaf_v0.6.1` teacher. The
    one-shot retrain against `vmaf_v1.0.16_3d0h` and the remaining tiny-AI
    training are RC9 work, see the [project roadmap](../roadmap.md).

## Status at a glance

| Item | Status | ADR | Evidence |
| --- | --- | --- | --- |
| Training (`ai/`, `vmaf-train`) | Shipped | [ADR-0020](../adr/0020-tinyai-four-capabilities.md) | [training.md](training.md) |
| Inference (`core/src/dnn/`, ONNX Runtime, op allowlist, 50 MB cap) | Shipped | [ADR-0023](../adr/0023-tinyai-user-surfaces.md) | [inference.md](inference.md) |
| Model signing verification (`--tiny-model-verify`) | Shipped | ADR-0211 | [security.md](security.md) |
| Model registry | Shipped, 26 entries | ADR-0211 | [model-registry.md](model-registry.md) |
| FFmpeg `vmafx` tiny options, `vmafx_pre` (8/10/12-bit, optional chroma) | Shipped | ADR-0482 | `ffmpeg-patches/0004`, `ffmpeg-patches/0005` |
| FR baselines (`fr_regressor_v1`, `vmaf_tiny_v2`) | Shipped | [ADR-0249](../adr/0249-fr-regressor-v1.md), [ADR-0244](../adr/0244-vmaf-tiny-v2.md) | [model cards](models/vmaf_tiny_v2.md) |
| `nr_metric_v1`, `learned_filter_v1` | Shipped | [ADR-0168](../adr/0168-tinyai-konvid-baselines.md) | [nr_metric_v1](models/nr_metric_v1.md), [learned_filter_v1](models/learned_filter_v1.md) |
| LPIPS-SqueezeNet FR extractor | Shipped | none | [lpips_sq](models/lpips_sq.md) |
| DISTS-Sq extractor | Shipped with smoke checkpoint; production weights pending (`T7-DISTS-followup`) | none | [dists_sq](models/dists_sq.md) |
| MobileSal scoring extractor, saliency students | Shipped; `saliency_student_v2` is the production default | [ADR-0218](../adr/0218-mobilesal-saliency-extractor.md), [ADR-0444](../adr/0444-saliency-student-v2-production-promotion.md) | [mobilesal](models/mobilesal.md) |
| `tools/vmaf-roi` encoder ROI sidecar | Shipped | none | [vmaf-roi](../usage/vmaf-roi.md) |
| TransNet V2 shot boundaries, `tools/vmaf-perShot` | Shipped | [ADR-0261](../adr/0261-transnet-v2-real-weights.md), [ADR-0222](../adr/0222-vmaf-per-shot-tool.md) | [transnet_v2](models/transnet_v2.md), [vmaf-perShot](../usage/vmaf-perShot.md) |
| FastDVDnet extractor (real weights) | Shipped; FFmpeg `vmafx_pre_temporal` filter planned | [ADR-0255](../adr/0255-fastdvdnet-pre-real-weights.md) | [fastdvdnet_pre](models/fastdvdnet_pre.md) |
| Allowlist `Loop` / `If` with bounded-iteration guard | Shipped | [ADR-0169](../adr/0169-onnx-allowlist-loop-if.md), [ADR-0171](../adr/0171-bounded-loop-trip-count.md) | [security.md](security.md) |
| `vmaf-train tune` (Optuna) | Shipped | none | [training.md](training.md#hyperparameter-sweeps) |
| FFmpeg `vmaf_post` filter | Planned, no patch number assigned | none | none |
| `describe_worst_frames` MCP tool | Planned | none | none |
| CLIP-IQA pseudo-labeler, KADID-10k pipeline, Ray tuning backend | Deferred | none | none |
| GPU-parity CI for tiny-AI EPs | Not implemented; cross-EP variance is checked manually | none | [inference.md](inference.md#expected-cross-device-variance) |
| `vmaf_tiny_v5` corpus expansion | Deferred | [ADR-0287](../adr/0287-vmaf-tiny-v5-corpus-expansion.md) | none |

## 1. Shipped baseline

The shipped surface:

- **Training.** `ai/` (PyTorch + Lightning) and the `vmaf-train` CLI.
- **Inference.** `core/src/dnn/`: the ONNX Runtime C API behind a 74-entry op
  allowlist, a model cap of at most 50 MB and a path-hardened loader.
- **C API.** `vmaf_use_tiny_model()` and `VmafDnnSession` open, run and close.
- **CLI.** `vmaf --tiny-model PATH --tiny-device STR`. The 12 device strings are
  listed in [inference.md](inference.md#execution-provider-matrix).
- **FFmpeg.** `ffmpeg-patches/0004` adds the tiny-model options to `vmafx`,
  and `ffmpeg-patches/0005` adds the `vmafx_pre` learned-filter filter.
- **Checkpoints.** `model/tiny/` holds 26 registry entries, see
  [model-registry.md](model-registry.md).
- **Signing.** `--tiny-model-verify` is wired to `cosign verify-blob`
  (ADR-0211 / T6-9). `registry.json` carries SHA-256 pins and Sigstore bundle
  paths.

Still outstanding: there is no GPU-parity CI, so cross-execution-provider
variance is verified manually.

## 2. Wave 1

All four sub-lists below were approved in the popup behind
[ADR-0107](../adr/0107-tinyai-wave1-scope-expansion.md) (a paraphrased
restatement of the original
[ADR-0036](../adr/0036-tinyai-wave1-scope-expansion.md)).
Shipping baselines was the blocker for everything else.

### 2.1 Ship baselines

| Model | Role | Status | Target / result |
| --- | --- | --- | --- |
| `fr_regressor_v1.onnx` | C1 FR | Shipped 2026-04-29 ([ADR-0249](../adr/0249-fr-regressor-v1.md)). The local Netflix Public drop unblocked the original deferral. | Mean LOSO PLCC vs `vmaf_v0.6.1` in `model/tiny/fr_regressor_v1.json`; ship gate is at least 0.95 |
| `vmaf_tiny_v2.onnx` | C1 FR (canonical-6 fusion) | Shipped 2026-04-29 ([ADR-0244](../adr/0244-vmaf-tiny-v2.md)). The 3-corpus parquet (Netflix, KoNViD, BVI-DVC D+C) closed the gap the Netflix-only regressor had left. | Netflix LOSO PLCC 0.9978 ± 0.0021 (9 folds x 5 seeds); KoNViD 5-fold PLCC 0.9998; about 257-parameter `mlp_small` with bundled StandardScaler |
| `nr_metric_v1.onnx` | C2 NR | Shipped 2026-04-25 ([ADR-0168](../adr/0168-tinyai-konvid-baselines.md)) | KoNViD-1k val/MSE 0.382 (RMSE about 0.62 on 1 to 5 MOS); MobileNet-tiny, about 19K parameters |
| `learned_filter_v1.onnx` | C3 filter | Shipped 2026-04-25 ([ADR-0168](../adr/0168-tinyai-konvid-baselines.md)) | KoNViD-1k self-supervised val/L1 0.019 on normalised luma; 4-block residual CNN, about 19K parameters |

The C2 and C3 first run exercised the full pipeline end to end:
`fetch_konvid_1k.py`, `vmaf-train manifest-scan`, `extract_konvid_frames.py`,
`train_konvid.py`, `export_tiny_models.py`, `model/tiny/registry.json`. C1
followed on 2026-04-29 once the Netflix Public Dataset was locally available.

### 2.2 LPIPS-SqueezeNet as an FR baseline

- **Why.** Industry-standard perceptual FR. It complements the homegrown C1 with
  an externally validated reference point. The SqueezeNet variant fits under the
  size cap (about 2.5M parameters plus about 1.25M frozen features).
- **Integration.** A feature extractor under `core/src/feature/` that calls
  `vmaf_dnn_session_*` and emits `lpips_sq` per frame next to VMAF's composite
  features.
- **ONNX.** Stock convolutions and global pooling, static input shape, opset 17,
  no custom ops. Upstream reference:
  [`richzhang/PerceptualSimilarity`](https://github.com/richzhang/PerceptualSimilarity).
- **Status.** Shipped.

### 2.3 DISTS-Sq as the LPIPS companion

- **Why.** Bristol VI-Lab's NVC audit flags DISTS as the deep-feature FR
  companion to LPIPS.
- **Integration.** `core/src/feature/feature_dists.c` mirrors LPIPS' two-input
  DNN
  session and emits `dists_sq` per frame.
- **Status.** The extractor ships with a smoke checkpoint. Production weights
  remain `T7-DISTS-followup`.

### 2.4 MobileSal: saliency-weighted VMAF and encoder ROI

One saliency model (about 2.5M parameters) feeds two surfaces:

1. **Scoring side.** Multiply the saliency map into the per-pixel residual
   before
   spatial pooling in existing feature extractors. This is the SVMAF variant
   published in academic work but never shipped.
2. **Encoder side.** Emit a per-CTU QP-offset map consumed by `x265 --qpfile` or
   the SVT-AV1 ROI API. Large bitrate win at fixed subjective quality.

Integration status:

- **`mobilesal` extractor** (scoring side, T6-2a). Emits a scalar
  `saliency_mean` per frame. It shipped with the historical smoke checkpoint
  first. Production use now points at the fork-trained
  [`saliency_student_v1`](models/saliency_student_v1.md) weights, and
  `mobilesal_placeholder_v0` stays as a registry smoke and legacy artefact. See
  [`models/mobilesal.md`](models/mobilesal.md) and
  [ADR-0218](../adr/0218-mobilesal-saliency-extractor.md).
- **`tools/vmaf-roi`** (encoder side, T6-2b). Shipped. It writes an ASCII grid
  for
  x265 (`--qpfile-style`) and raw `int8_t` binary for SVT-AV1
  (`--roi-map-file`), accepts 8/10/12/16-bit planar YUV and handles one frame
  per
  invocation. See [`docs/usage/vmaf-roi.md`](../usage/vmaf-roi.md). Wave-2
  follow-ups are a multi-frame batch mode and `--blend edge-density`.
- **Evaluation.** [`eval_saliency_per_mb.py`](saliency-per-mb-eval.md) reports
  IoU
  after reducing masks to the block grids the encoder ROI paths consume. Use it
  before promoting a temporal or video-saliency model.

The upstream MobileSal swap is no longer the production path: ADR-0257 records
the
CC BY-NC-SA, Google-Drive and RGB-D blockers. The production path is the
fork-trained DUTS saliency student, with the same `input` / `saliency_map`
tensor
contract as the placeholder. `saliency_student_v2` is the production default
since
2026-05-15 (IoU 0.7105 against v1's 0.6558, +8.3%;
[ADR-0444](../adr/0444-saliency-student-v2-production-promotion.md)). Use
`model/tiny/saliency_student_v2.onnx` for new encodes. `saliency_student_v1` is
kept for regression baselines.

### 2.5 Per-shot CRF predictor and TransNet V2 shot boundaries

- **Why.** Content-adaptive encoding without an ML framework in the encoder.
  These are the smallest models on this roadmap (under 1M each), with
  disproportionate bitrate-at-quality savings.
- **Pipeline.** (1) TransNet V2 (about 1M) produces per-frame shot-change
  scores,
  which become a list of shot timestamps. (2) A per-shot CRF predictor takes a
  downsampled thumbnail plus classical features (motion energy, spatial
  complexity) and predicts the CRF that hits the target VMAF on that shot.
- **Integration.** The standalone CLI `tools/vmaf-perShot` writes an
  encoder-ingestible sidecar. It does not run inside libvmaf: its output is a
  parameter hint, not a quality score.

Status:

- **Shot-boundary extractor shipped** (T6-3a, 2026-04-29, real upstream weights
  in
  T6-3a-followup, [ADR-0261](../adr/0261-transnet-v2-real-weights.md)). The
  `transnet_v2` extractor runs upstream's 100-frame windows
  ([ADR-1527](../adr/1527-transnet-v2-upstream-windows.md)) with the
  `[1, 100, 3, 27, 48] -> [1, 100]` ONNX contract and the Soucek and Lokoc 2020
  MIT checkpoint at `model/tiny/transnet_v2.onnx` (`smoke: false`), wrapped by
  the
  fork's NTCHW adapter. It emits per-frame `shot_boundary_probability` and
  `shot_boundary` flags. See [`models/transnet_v2.md`](models/transnet_v2.md).
- **`tools/vmaf-perShot` shipped** (T6-3b, 2026-04-29,
  [ADR-0222](../adr/0222-vmaf-per-shot-tool.md)), see
  [`vmaf-perShot`](../usage/vmaf-perShot.md). v1 uses a transparent linear-blend
  predictor and a frame-difference shot detector fallback. TransNet V2 is
  available as the libvmaf feature extractor for pipelines that consume
  feature-collector output directly.
- **v2 deferred.** It will swap the linear blend for a small trained MLP under
  the
  same CSV / JSON schema, under a separate ADR, once a labelled per-shot CRF
  corpus exists.

## 3. FFmpeg and encoder expansion

The slots below are not filled by the current `ffmpeg-patches/` series.

### 3.1 `vmafx_pre`: 10-bit and chroma (shipped)

`ffmpeg-patches/0005` ("add vmafx_pre filter")
accepts 8, 10 and 12-bit planar 4:2:0, 4:2:2 and 4:4:4 input, and filters the
U/V
planes when `chroma=1` (default 0, luma only). Chroma and HDR sources are where
classical pre-filters leave the most budget on the table. Filter options are in
[inference.md](inference.md#surface-3-ffmpeg-filters).

### 3.2 New `vmafx_post` filter (planned)

- **Why.** Today the pair (reference, distorted) is scored. A post filter would
  score the actually decoded stream inside an ffmpeg pipeline with the C2 NR
  model, sharing the backbone with the in-tree NR metric.
- **Integration.** A new patch in the series, with a filter mirroring
  `vmafx_pre`'s shape: frame in, score out, no frame out (measurement only).

### 3.3 FastDVDnet temporal pre-filter

- **Why.** A published temporal denoise CNN (about 2.5M parameters, 5-frame
  window). Denoise-before-encode is a well-validated bitrate lever for noisy or
  grainy sources.
- **Cost.** The filter needs a 5-frame buffer, a bigger lift than per-frame
  filters. Deferred if Wave 1 is too wide.
- **Integration.** A new `vmafx_pre_temporal` filter, or a mode flag on
  `vmafx_pre`.
- **Status.** The extractor is shipped (T6-7, 2026-04-29, real upstream weights
  in
  T6-7b, [ADR-0255](../adr/0255-fastdvdnet-pre-real-weights.md)). The
  `fastdvdnet_pre` extractor uses a 5-slot ring buffer with the
  `[1, 5, H, W] -> [1, 1, H, W]` ONNX contract and the real
  m-tassano/FastDVDnet checkpoint under `model/tiny/fastdvdnet_pre.onnx`
  (`smoke: false`), wrapped by the fork's luma adapter. The FFmpeg
  `vmafx_pre_temporal` filter that consumes the denoised frame buffer remains to
  be written. See [`models/fastdvdnet_pre.md`](models/fastdvdnet_pre.md).

## 4. Op-allowlist expansion: bounded `Loop` and `If` (shipped)

Decision: whitelist `Loop` and `If` with a bounded-iteration guard. Published
transformer and optical-flow architectures that target ONNX export have bounded
loops, and unbounded loops are a sandbox risk (infinite compute, adversarial
model). The allowlist entries, the 1024 trip-count cap at export time and the
16-node, depth-8 caps at load time are described in
[security.md](security.md#layer-1-operator-allowlist)
([ADR-0169](../adr/0169-onnx-allowlist-loop-if.md),
[ADR-0171](../adr/0171-bounded-loop-trip-count.md)).

The expansion unlocks:

- **MUSIQ** (about 27M): NR transformer with multi-scale attention.
- **RAFT-Small** (about 1M): optical flow with an iterative GRU update.
- **Small VLMs** (SmolVLM 256M family): transformer decoder.

Non-goal: `Scan`, whose more expressive iteration semantics would need a much
larger analysis pass.

## 5. MCP and LLM surfaces

### 5.1 `describe_worst_frames` MCP tool (planned)

When VMAF says "frame 847 is bad", the user still has to open the frame to see
why. A local VLM closes that loop in plain English, for example "underexposed in
the foreground; mild banding on the sky gradient". It is a debugging affordance,
not a scoring component.

Implementation: a new method in `mcp-server/vmaf-mcp/`, taking a VMAF JSON
output
path and N.

1. Pick the N frames with the largest VMAF delta from the per-frame scores.
2. Extract those frames as PNGs, reusing ffmpeg.
3. Run SmolVLM (about 256M) locally with a prompt template that asks for
   artifact
   types and plausible causes.
4. Return a JSON list of `{frame_index, vmaf, caption}`.

Model choice is the SmolVLM family. If the 256M variant misses, fall back to
Moondream2 (1.8B quantized Q4, fits in 4 GB VRAM). The VLM runs through ONNX
Runtime under the extended allowlist (section 4). Absolute path resolution and
the 50 MB cap still apply. Larger VLMs need the compile-time
`VMAF_DNN_DEFAULT_MAX_BYTES` constant in
[`core/src/dnn/model_loader.h`](../../core/src/dnn/model_loader.h) bumped and the
library rebuilt (the historical `VMAF_MAX_MODEL_BYTES` environment override was
retired in T7-12).

## 6. Training-side items

Not in Wave 1, listed so they are not forgotten:

- **CLIP-IQA pseudo-labeler.** Offline bootstrap for NR datasets.
- **KADID-10k synthetic distortion pipeline.** Classical augmentation.
- **Hyperparameter-tuning Ray backend.** Once `tune` stabilizes.

`vmaf-train tune` (Optuna) is implemented, see
[training.md](training.md#hyperparameter-sweeps).

## 7. Infrastructure items

- **GPU-parity CI** (outstanding). CPU against CUDA and CPU against OpenVINO
  cross-device variance as a required status check (at most 1e-4 FP32 and 1e-2
  FP16, per [`inference.md`](inference.md#expected-cross-device-variance)).
- **Sigstore verification** (shipped, ADR-0211). `--tiny-model-verify` is wired
  to
  `cosign verify-blob`; production deployments should set it on.
- **Model registry** (shipped, ADR-0211). `model/tiny/registry.json` carries
  SHA-256 pins, Sigstore bundle paths and license metadata for all 26 entries.
  See
  [model-registry.md](model-registry.md).

## 8. Out of scope

- Training inside libvmaf. ML framework dependencies stay in `ai/` and Python.
- A second inference runtime (TFLite, ggml). ONNX Runtime is the one runtime.
- Cloud-only or API-dependent models. Everything runs locally.
- Models larger than 50 MB. The cap is the compile-time
  `VMAF_DNN_DEFAULT_MAX_BYTES` constant; bump it in
  [`core/src/dnn/model_loader.h`](../../core/src/dnn/model_loader.h) and rebuild
  when a use case genuinely needs it.
- `Scan` and arbitrary control flow, see section 4.

## 9. Related documents

- [overview.md](overview.md): the four existing capabilities.
- [training.md](training.md): `vmaf-train` CLI and dataset flow.
- [inference.md](inference.md): CLI, C API and ffmpeg surfaces.
- [benchmarks.md](benchmarks.md): PLCC/SROCC/RMSE methodology.
- [security.md](security.md): op allowlist and size cap.
- [ADR-0107](../adr/0107-tinyai-wave1-scope-expansion.md): this roadmap's
  authority (supersedes
  [ADR-0036](../adr/0036-tinyai-wave1-scope-expansion.md)).
