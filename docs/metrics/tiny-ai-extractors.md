<!-- markdownlint-disable MD060 -->
# Tiny-AI feature extractors

Five feature extractors run a small ONNX model through ONNX Runtime instead of
a hand-written kernel: `lpips`, `dists_sq`, `fastdvdnet_pre`, `mobilesal` and
`transnet_v2`. Give each the model with the `model_path` option or an
environment variable, for example
`--feature lpips=model_path=/path/to/lpips.onnx`. [features](features.md)
lists every extractor.

## Shared behaviour

| Extractor | Output metrics | Model path option | Environment variable fallback | Reads |
| --- | --- | --- | --- | --- |
| `lpips` | `lpips` | `model_path` | `VMAF_LPIPS_MODEL_PATH` | reference and distorted |
| `dists_sq` | `dists_sq` | `model_path` | `VMAF_DISTS_SQ_MODEL_PATH` | reference and distorted |
| `fastdvdnet_pre` | `fastdvdnet_pre_l1_residual` | `model_path` | `VMAF_FASTDVDNET_PRE_MODEL_PATH` | reference, 5-frame window |
| `mobilesal` | `saliency_mean` | `model_path` | `VMAF_MOBILESAL_MODEL_PATH` | distorted only, 8-bit |
| `transnet_v2` | `shot_boundary_probability`, `shot_boundary` | `model_path` | `VMAF_TRANSNET_V2_MODEL_PATH` | distorted only, 100-frame window |

The option wins over the environment variable. All five share these rules:

- **Backends** — scalar only on the libvmaf side. The ONNX model runs on the
  ORT execution provider selected with `--tiny-device` (CPU, CUDA, OpenVINO,
  ROCm); see [`docs/ai/inference.md`](../ai/inference.md).
- **Without DNN support** — a libvmaf built without ORT returns `-ENOSYS` from
  init, before it probes the model path.
- **Without a model** — a DNN-enabled build returns `-EINVAL` when neither the
  option nor the environment variable is set.
- **Runtime** — depends on the [tiny-AI runtime](../ai/overview.md). The model
  registry [`model/tiny/registry.json`](../../model/tiny/registry.json) tracks
  the canonical checkpoints and marks smoke placeholders with `smoke: true`.

## LPIPS — learned perceptual image patch similarity

A perceptual-distance metric backed by an ONNX model with two image
inputs (`ref`, `dist`). Distinct from the classic VMAF feature
extractors in that the heavy lifting is delegated to ONNX Runtime via
the tiny-AI surface. The model is loaded once at extractor init and
runs per frame.

### Invocation

- CLI: `--feature lpips=model_path=/path/to/lpips.onnx`.
- ffmpeg: `libvmaf=feature=name=lpips:model_path=...`.
- C API: `vmaf_use_feature(ctx, "lpips", opts)` with
  `model_path` set on the dictionary.

**Output metrics** — `lpips` (one scalar per frame). Lower is more
similar.

**Output range** — model-defined; the reference LPIPS network produces
values in roughly `[0, 1]` for natural content but is not bounded by
construction.

**Input formats** — YUV 4:2:0 / 4:2:2 / 4:4:4, 8 / 10 / 12 / 16 bpc.
4:0:0 is rejected (chroma is required for the RGB conversion).
High-bit-depth inputs are rounded into the same 8-bit RGB tensor contract
used by the shipped LPIPS checkpoint.

### Options

| Option       | Type   | Default | Effect                                                                                                                         |
|--------------|--------|---------|--------------------------------------------------------------------------------------------------------------------------------|
| `model_path` | string | unset   | Filesystem path to the LPIPS ONNX model (two-input). If unset, falls back to the `VMAF_LPIPS_MODEL_PATH` environment variable. |

**Backends** — scalar only on the libvmaf side (the ONNX model itself
is dispatched to whichever ORT execution provider is selected via
`--tiny-device`; see [`docs/ai/inference.md`](../ai/inference.md)).

**Limitations** — depends on the
[tiny-AI runtime](../ai/overview.md). On builds compiled without DNN
support, init returns `-ENOSYS` before model-path probing. On DNN-enabled
builds, the extractor errors out with `-EINVAL` if no model path is
provided (neither the option nor the environment variable); the registry under
[`model/tiny/registry.json`](../../model/tiny/registry.json) tracks
the canonical LPIPS ONNX checkpoint.

## DISTS-Sq — deep image structure and texture similarity

See also the [DISTS-Sq page](dists.md).

A DISTS-shaped full-reference perceptual-distance extractor backed by a
two-input ONNX model. It shares the LPIPS host pipeline: YUV frame pairs are
converted to ImageNet-normalised RGB tensors, passed to ONNX Runtime via the
tiny-AI DNN surface, and collected as one scalar per frame.

The shipped `model/tiny/dists_sq.onnx` checkpoint is a smoke placeholder. It
computes mean squared distance between the two normalised RGB tensors so the
extractor ABI and runtime path are testable before the real DISTS weights land.

### Invocation

- CLI: `--feature dists_sq=model_path=/path/to/dists_sq.onnx`.
- ffmpeg: `libvmaf=feature=name=dists_sq:model_path=...`.
- C API: `vmaf_use_feature(ctx, "dists_sq", opts)` with
  `model_path` set on the dictionary.

**Output metrics** — `dists_sq` (one scalar per frame). Lower is more
similar.

**Output range** — placeholder-defined non-negative distance. It is not
calibrated to published DISTS values.

**Input formats** — YUV 4:2:0 / 4:2:2 / 4:4:4, 8 / 10 / 12 / 16 bpc.
4:0:0 is rejected because chroma is required for RGB conversion.
High-bit-depth inputs are rounded into the same RGB8 tensor contract as
LPIPS before ONNX inference.

### Options

| Option       | Type   | Default | Effect                                                                                                                             |
|--------------|--------|---------|------------------------------------------------------------------------------------------------------------------------------------|
| `model_path` | string | unset   | Filesystem path to the DISTS-Sq ONNX model (two-input). If unset, falls back to `VMAF_DISTS_SQ_MODEL_PATH` environment variable.   |

**Backends** — scalar only on the libvmaf side. ONNX execution follows the
configured tiny-AI provider selected via `--tiny-device`.

**Limitations** — depends on the [tiny-AI runtime](../ai/overview.md).
On builds compiled without DNN support, init returns `-ENOSYS` before
model-path probing; on DNN-enabled builds, missing `model_path` returns
`-EINVAL`. The committed checkpoint is marked
`smoke: true` in the model registry and should be used only for smoke tests
until the real DISTS weights replace it.

## FastDVDnet pre — temporal denoising pre-filter

A *temporal* denoising pre-filter backed by an ONNX model with a
single input tensor stacking five luma planes
``[t-2, t-1, t, t+1, t+2]`` along the channel axis; the network emits
a denoised version of frame ``t``. Structurally a feature extractor
(registered in `feature_extractor_list[]` and discoverable by name)
but logically a *pre-filter*, not a quality metric — denoise-before-
encode is a bitrate lever, not a score. Runs through ORT once at
init; per-frame inference uses a 5-slot ring buffer of float32 luma
planes with reflection-pad-light end behaviour.

See also [`docs/ai/models/fastdvdnet_pre.md`](../ai/models/fastdvdnet_pre.md),
[ADR-0215](../adr/0215-fastdvdnet-pre-filter.md), and
[ADR-0255](../adr/0255-fastdvdnet-pre-real-weights.md) for the full
surface contract, placeholder history, and real-weight export.

### Invocation

- CLI: `--feature fastdvdnet_pre=model_path=/path/to/fastdvdnet_pre.onnx`.
- ffmpeg: `libvmaf=feature=name=fastdvdnet_pre:model_path=...`.
- C API: `vmaf_use_feature(ctx, "fastdvdnet_pre", opts)` with
  `model_path` set on the dictionary.

**Output metrics** — `fastdvdnet_pre_l1_residual` (one scalar per
frame): mean-absolute difference between the centre frame ``t``
(normalised to `[0, 1]`) and the denoised output. Exists so libvmaf's
per-frame plumbing has a scalar to record; **not** a quality metric.
Downstream pipelines that want the actual denoised pixel data should
consume the FFmpeg `vmaf_pre_temporal` filter once that follow-up
lands; the current extractor records the diagnostic residual only.

**Output range** — `[0.0, 1.0]` by construction (mean-absolute on
normalised luma). Typical values: `~0.0` for quiet / flat content,
`~0.05` for lightly noisy content, and `~0.20+` on heavy denoising or
saturated inputs.

**Input formats** — YUV 4:2:0 / 4:2:2 / 4:4:4, 8 / 10 / 12 / 16 bpc.
Y plane only (chroma is ignored).

### Options

| Option       | Type   | Default | Effect                                                                                                                                                                      |
|--------------|--------|---------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `model_path` | string | unset   | Filesystem path to the FastDVDnet ONNX model (5-frame `frames` input, single-frame `denoised` output). Overrides the `VMAF_FASTDVDNET_PRE_MODEL_PATH` environment variable. |

**Backends** — scalar only on the libvmaf side (the ONNX model
itself is dispatched to whichever ORT execution provider is selected
via `--tiny-device`; see [`docs/ai/inference.md`](../ai/inference.md)).

Limitations:

- Stateful: a 5-frame sliding window.
- Not a metric: `fastdvdnet_pre_l1_residual` is a diagnostic residual, not a
  perceptual score.
- Without ORT, or without a model path, init fails as described in
  [Shared behaviour](#shared-behaviour).
- The shipped checkpoint `model/tiny/fastdvdnet_pre.onnx` carries real upstream
  m-tassano/FastDVDnet weights (`smoke: false` in `model/tiny/registry.json`)
  wrapped by the ADR-0255 luma adapter. The wrapper tiles Y into RGB, supplies
  the fixed `sigma = 25/255` noise map, and collapses the RGB output back to
  BT.601 luma while preserving the C extractor's `[1, 5, H, W] -> [1, 1, H, W]`
  ONNX contract.
- Remaining follow-ups are the FFmpeg `vmaf_pre_temporal` consumer filter and a
  luma-native retrain; the model shipped here is not the old smoke-only
  placeholder.

## `mobilesal` — MobileSal saliency map

Runs the MobileSal RGB saliency network on each distorted frame and
emits a per-frame saliency mean. Companion ADR
[`docs/adr/0218-mobilesal-saliency-extractor.md`](../adr/0218-mobilesal-saliency-extractor.md)
records the extractor design + the historical synthetic-placeholder
ONNX. Production use should select the fork-trained
`model/tiny/saliency_student_v1.onnx` checkpoint; the placeholder
remains in `model/tiny/registry.json` with `smoke: true` for legacy
and pipeline smoke coverage. The encoder-side `tools/vmaf-roi` sidecar
is shipped and consumes the same saliency-map contract for per-CTU QP
offsets.

### Invocation

- CLI: `--feature mobilesal=model_path=/path/to/mobilesal.onnx`.
- ffmpeg: `libvmaf=feature=name=mobilesal:model_path=...`.
- C API: `vmaf_use_feature(ctx, "mobilesal", opts)` with
  `model_path` set on the dictionary.

**Output metrics** — `saliency_mean` (one scalar per frame: mean saliency
across the H×W output map).

**Backends** — scalar only on the libvmaf side; ORT-dispatched to the
selected execution provider.

**Model path** — `model_path`, or the `VMAF_MOBILESAL_MODEL_PATH` environment
variable when the option is unset.

**Limitations** — the default historical `mobilesal.onnx` placeholder is
smoke-only; use `saliency_student_v1.onnx` for content-dependent saliency. The
C extractor accepts 8-bit YUV only and rejects other bit depths at init;
high-bit-depth input support is available on the encoder-side `vmaf-roi` tool,
not on the scoring-side `mobilesal` feature yet.

## `transnet_v2` — TransNet V2 shot-boundary detector

Runs the TransNet V2 shot-boundary detector on a sliding 100-frame
window of 27x48 RGB thumbnails (downsampled from the distorted
stream's luma + reconstructed chroma) and emits a per-frame shot-
boundary probability plus a thresholded binary flag. Companion ADRs
[`docs/adr/0223-transnet-v2-shot-detector.md`](../adr/0223-transnet-v2-shot-detector.md)
and
[`docs/adr/0261-transnet-v2-real-weights.md`](../adr/0261-transnet-v2-real-weights.md)
record the extractor contract and the real upstream Soucek & Lokoc
2020 weights drop. The per-shot CRF predictor that consumes these
features is T6-3b.

### Invocation

- CLI: `--feature transnet_v2=model_path=/path/to/transnet_v2.onnx`.
- ffmpeg: `libvmaf=feature=name=transnet_v2:model_path=...`.
- C API: `vmaf_use_feature(ctx, "transnet_v2", opts)` with
  `model_path` set on the dictionary.

**Output metrics** — `shot_boundary_probability` (sigmoid of the most
recent frame's boundary logit, range `[0.0, 1.0]`) and `shot_boundary`
(binary flag `0.0` / `1.0`, thresholded at `0.5` against
`shot_boundary_probability`). Downstream consumers (per-shot CRF
predictor T6-3b, FFmpeg shot-cut filter) bind to these two names.

**Backends** — scalar only on the libvmaf side; ORT-dispatched to
the selected execution provider.

Limitations:

- Stateful: a 100-frame sliding window. The first 99 frames emit boundary
  probabilities computed against a partially filled window.
- Without ORT, or without a model path, init fails as described in
  [Shared behaviour](#shared-behaviour).
- The shipped checkpoint `model/tiny/transnet_v2.onnx` carries real upstream
  soCzech/TransNetV2 weights (`smoke: false` in `model/tiny/registry.json`)
  wrapped by the ADR-0261 NTCHW adapter. The wrapper preserves the C
  extractor's `[1, 100, 3, 27, 48] -> [1, 100]` ONNX contract while invoking
  the upstream NTHWC graph and selecting the boundary-logits output.
- Remaining follow-ups are per-shot CRF aggregation and true RGB / bilinear
  thumbnail input; the model shipped here is not the old smoke-only
  placeholder.
