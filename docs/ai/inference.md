# Tiny AI — inference

Three consumer surfaces share one runtime: `vmaf` CLI, libvmaf C API, and
ffmpeg filters. All three funnel through
[`core/src/dnn/ort_backend.c`](../../core/src/dnn/ort_backend.c).

## Prerequisites

- libvmaf built with `-Denable_dnn=enabled` (or `auto` with ONNX Runtime
  discoverable via `pkg-config`).
- ONNX Runtime ≥ 1.20 available at build time. ONNX Runtime isn't in the
  distro setup scripts under [scripts/setup/](../../scripts/setup/) yet —
  install the prebuilt release tarball from
  <https://github.com/microsoft/onnxruntime/releases> or a distro package
  if available, and make sure its `libonnxruntime.so` + headers are on
  `PKG_CONFIG_PATH` before running `meson setup`.
- A `.onnx` model + sidecar `.json` pair under `model/tiny/` or anywhere
  else — the CLI flag accepts an absolute path.

Verify at runtime:

```bash
vmaf --help | grep -- '--tiny-model'   # must list the flag
vmaf --tiny-model /missing.onnx 2>&1   # should print a clear error,
                                       # not "option not found"
```

Optional deployment hardening:

| Environment | Default | Notes |
| --- | --- | --- |
| `VMAF_TINY_MODEL_DIR` | unset | Directory jail for `--tiny-model` / libvmaf tiny-model loads. When set, every ONNX path is resolved with symlinks followed and must live below this directory before the loader stats or maps the file. Missing, non-directory, sibling-prefix, and symlink-escape paths fail closed with `-EACCES`. |

Example:

```bash
export VMAF_TINY_MODEL_DIR=/opt/vmaf-models
vmaf -r ref.yuv -d dis.yuv -w 1920 -h 1080 -p 420 -b 8 \
     --tiny-model /opt/vmaf-models/vmaf_tiny_v2.onnx
```

## Surface 1 — the `vmaf` CLI

```bash
# C1 — drop-in augmentation of the classic SVM. Default tiny FR model
# is now vmaf_tiny_v2 (ADR-0244) — supersedes the prior vmaf_tiny_v1.
vmaf -r ref.yuv -d dis.yuv -w 1920 -h 1080 -p 420 -b 8 \
     -m version=vmaf_v0.6.1 \
     --tiny-model model/tiny/vmaf_tiny_v2.onnx \
     --tiny-device cuda

# C2 — no-reference.
vmaf -d dis.yuv -w 1920 -h 1080 -p 420 -b 8 \
     --tiny-model model/tiny/vmaf_nr_mobilenet_v1.onnx \
     --no-reference
```

> **Default flip (2026-04-29).** `vmaf_tiny_v2` replaces
> `vmaf_tiny_v1` as the recommended tiny FR fusion model. Same input
> contract (canonical-6 features), same output range (0–100 VMAF),
> +0.005–0.018 PLCC across the Phase-3 validation chain. The v1 file
> stays on disk as a regression baseline. See
> [`models/vmaf_tiny_v2.md`](models/vmaf_tiny_v2.md) for the full
> model card.
>
> **`vmaf_tiny_v3` available alongside v2 (2026-05-02, ADR-0241).**
> A wider/deeper variant (`mlp_medium` 6 → 32 → 16 → 1, ~769 params)
> trained on the same 4-corpus parquet, same recipe. Netflix LOSO
> mean PLCC 0.9986 ± 0.0015 vs v2's 0.9978 ± 0.0021 (+0.0008 mean,
> -30 % std). v2 remains the production default; pick v3 for
> lowest-variance estimates. See [`models/vmaf_tiny_v3.md`](models/vmaf_tiny_v3.md).

**Architecture ladder (2026-05-02).** The tiny VMAF fusion family
now spans three rungs sharing the canonical-6 input contract:

| Model | Arch | Params | ONNX | NF LOSO PLCC | Status |
| --- | --- | ---: | ---: | ---: | --- |
| [`vmaf_tiny_v2`](models/vmaf_tiny_v2.md) | mlp_small | 257 | 2.5 KB | 0.9978 ± 0.0021 | **Production default** |
| [`vmaf_tiny_v3`](models/vmaf_tiny_v3.md) | mlp_medium | 769 | 4.5 KB | 0.9986 ± 0.0015 | Opt-in (recommended higher tier) |
| [`vmaf_tiny_v4`](models/vmaf_tiny_v4.md) | mlp_large | 3073 | 14.0 KB | 0.9987 ± 0.0015 | Opt-in (top of measured ladder) |

v4's PLCC win over v3 is +0.0001 (below 1 std) — the ladder
saturates on the canonical-6 + 4-corpus regime. ADR-0242 records
"the arch ladder stops here". Pick v3 unless you specifically want
the absolute top of the measured ladder; pick v2 for the smallest
bundle.

New flags:

| Flag | Default | Notes |
| --- | --- | --- |
| `--tiny-model PATH` | — | ONNX model path; sidecar JSON at `${PATH%.onnx}.json`. |
| `--tiny-device STR` | `auto` | `auto` \| `cpu` \| `cuda` \| `openvino` \| `coreml` \| `coreml-ane` \| `coreml-gpu` \| `coreml-cpu` \| `openvino-npu` \| `openvino-cpu` \| `openvino-gpu` \| `rocm`. |
| `--tiny-threads N` | `0` | CPU EP intra-op threads; 0 = ORT default. |
| `--tiny-fp16` | off | Request fp16 I/O when the EP supports it. |
| `--tiny-model-verify` | off | Require Sigstore-bundle verification (`cosign verify-blob`) before model load. Refuses to load on missing bundle, missing `cosign`, or non-zero exit. See [model-registry.md](model-registry.md) and [security.md](security.md). |
| `--tiny-codec NAME` | `unknown` | Encoder name for codec-aware tiny models (e.g. `fr_regressor_v2`). Validated against the sidecar's `encoder_vocab`; unknown names hard-fail at attach time so typos are caught. Common ffprobe aliases (`h264`, `hevc`, `av1`, `vp9`, `vvc`) are accepted. See [ADR-0522](../adr/0522-tiny-codec-preset-crf-cli-flags.md). |
| `--tiny-preset STR` | medium | Encoder preset (`medium` / `slow` / `p4` / `5` etc.); interpretation is encoder-specific and mirrors `ai/scripts/train_fr_regressor_v2.py::PRESET_ORDINAL`. Unknown presets fall back to ordinal 5. |
| `--tiny-crf N` | `0` | CRF / QP integer used during encoding; clamped to `[0, 63]` and normalised by 63 to match the trainer. |
| `--no-reference` | off | Skip reference loading; only valid with an NR tiny model. |

### Codec-aware tiny models (`fr_regressor_v2`)

`fr_regressor_v2.onnx` carries a second `codec` input of shape `[batch,
N_VOCAB + 2]`. The first `N_VOCAB` slots are a one-hot over the
sidecar's `encoder_vocab` (v2: 12 entries — `libx264`, `libx265`,
`libsvtav1`, `libvvenc`, `libvpx-vp9`, `h264_nvenc`, `hevc_nvenc`,
`av1_nvenc`, `h264_qsv`, `hevc_qsv`, `av1_qsv`, `unknown`); the last
two are `preset_norm = preset_ordinal / 9.0` and `crf_norm = crf / 63.0`.

Without `--tiny-codec` / `--tiny-preset` / `--tiny-crf` the loader
pre-seeds the codec block to the `unknown` baseline (ADR-0518) and the
model receives a constant conditioning vector — every call returns the
same score regardless of the distorted YUV's encoder. Passing the
flags populates the block from the user-supplied parameters via the
new `vmaf_dnn_set_codec_context()` public API (ADR-0519):

```bash
vmaf --reference src.yuv --distorted dst.yuv \
     --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
     --tiny-model model/tiny/fr_regressor_v2.onnx \
     --tiny-codec libx264 --tiny-preset medium --tiny-crf 28 \
     --json --output /tmp/scores.json
```

Unknown codec names exit non-zero before the first frame is read:

```text
$ vmaf … --tiny-codec UNKNOWN_ENC …
--tiny-codec 'UNKNOWN_ENC' not found in model encoder_vocab;
use one of the names listed by --help.
```

Non-codec-aware models (`fr_regressor_v1`, `vmaf_tiny_v4`, `dists_sq`)
reject the flags with a `-ENOTSUP` message — `--tiny-codec` requires a
model whose sidecar carries an `encoder_vocab` array.

`--tiny-model` accepts an absolute or relative path. For production,
set `VMAF_TINY_MODEL_DIR` to the trusted model directory and pass paths
inside that directory; a model outside the jail fails before ONNX
Runtime opens a session. The jail is independent of
`--tiny-model-verify`: use the jail to restrict *where* models may load
from, and verification to pin *which* signed model bytes may load.

Output JSON gains a `tiny_model` block alongside `pooled_metrics`:

```json
{
  "pooled_metrics": { "vmaf": { "mean": 91.23... } },
  "tiny_model": {
    "name": "vmaf_tiny_fr_v1",
    "kind": "fr",
    "device": "cuda",
    "mean": 90.8...,
    "per_frame": [...]
  }
}
```

For attached multi-output models, each scalar ONNX output is recorded as its own
feature. A single-output model keeps the sidecar `name` as the score key.
Multi-output models use `<sidecar-name>_<output-name>`, with `output-name`
taken from sidecar `output_names[]` when present and count-matched, otherwise
from the ONNX graph output name. Attached mode still rejects non-scalar output
tensors; use `vmaf_dnn_session_run()` when the caller needs vectors or images.

### Auto-resize for image-input tiny models (ADR-0550)

Image-input (rank-4 NCHW) tiny models declare a *fixed* input shape — the
shipped `model/tiny/nr_metric_v1.onnx` NR scorer, for example, expects
`[1, 1, 224, 224]` because it was trained on KoNViD-1k middle-frames
downscaled to 224×224 grayscale. Most NR workflows pass the encoder's
native resolution as `--width / --height`, so a dimension mismatch is
the norm rather than the exception.

The per-frame NCHW dispatch can **auto-resample the luma plane** to the
model's input shape when they differ. The default behaviour is
**`disabled`** — a dimension mismatch returns `-ERANGE` and the operator
must explicitly choose a resize filter. This preserves the strict mode
for parity harnesses and avoids a silent free parameter.

> **Warning:** `bilinear`, `nearest`, and `bicubic` produce scores that
> differ by approximately **2%** on the same input. Treat filter choice
> as a model hyperparameter and document it alongside the model
> checkpoint.

Filter selectors:

```text
--tiny-resize disabled   # default: mismatch -> -ERANGE (strict mode)
--tiny-resize bilinear   # torchvision / OpenCV BILINEAR (half-pixel-centre)
--tiny-resize nearest    # nearest-neighbour, floor coord (debug-friendly)
--tiny-resize bicubic    # Catmull-Rom (a = -0.5), separable
```

When the source dims already equal the model dims, the dispatch
forwards verbatim to `vmaf_tensor_from_luma` — the matched-dims path
stays bit-identical to the pre-ADR-0550 code, so the Netflix golden
gate is unaffected regardless of the selected filter.

Smoke test (Finding 11 reproducer — requires explicit `--tiny-resize`):

```bash
vmaf --no-reference \
     --tiny-model model/tiny/nr_metric_v1.onnx \
     --distorted testdata/dis_576x324_48f.yuv \
     --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
     --tiny-resize bilinear \
     --json --output /tmp/nr.json
# Expected: 48 frames scored, vmaf_tiny_model mean ~ 3.09 (bilinear),
# ~ 3.05 (nearest), ~ 3.11 (bicubic).
```

Without `--tiny-resize`, the default (disabled) produces 0 frames and
"problem reading pictures" for any size-mismatched NR model:

```bash
vmaf --no-reference \
     --tiny-model model/tiny/nr_metric_v1.onnx \
     --distorted testdata/dis_576x324_48f.yuv \
     --width 576 --height 324 --pixel_format 420 --bitdepth 8
# Expected: "problem reading pictures" at frame 0; 0 frames scored.
```

The same selector is reachable from the C API via
`vmaf_dnn_set_resize_mode(ctx, VMAF_DNN_RESIZE_BILINEAR | _NEAREST |
_BICUBIC | _DISABLED)` — see Surface 2 below.

## Surface 2 — the libvmaf C API

```c
#include <libvmaf/libvmaf.h>
#include <libvmaf/dnn.h>

VmafContext *ctx;
vmaf_init(&ctx, (VmafConfiguration){ /* ... */ });

if (!vmaf_dnn_available()) {
    fprintf(stderr, "libvmaf built without --enable_dnn; rebuild.\n");
    return 1;
}

VmafDnnConfig dnn_cfg = {
    .device       = VMAF_DNN_DEVICE_CUDA,
    .device_index = 0,
    .threads      = 0,
    .fp16_io      = false,
};
int err = vmaf_use_tiny_model(ctx, "/models/vmaf_tiny_fr_v1.onnx", &dnn_cfg);
if (err < 0) { /* handle -errno */ }

/* … feed frames as usual; tiny-model scores appear in the same
     per-frame collector the built-in SVM uses. */
```

The sidecar JSON is discovered automatically at
`${onnx_path%.onnx}.json`. Its `kind` field (`fr` / `nr`) tells libvmaf
whether to expect a reference.
Optional `output_names[]` entries name attached multi-output scalar scores; the
legacy `output_name` field remains accepted for single-output metadata.

### Accepted ONNX input shapes (ADR-0518, extended by ADR-0523)

The loader accepts two input ranks:

| Rank | Shape | Meaning | Example checkpoint |
| --- | --- | --- | --- |
| 4 | `[N, 1, H, W]` | NCHW single-channel luma image — the picture's Y plane is fed through `vmaf_tensor_from_luma` each frame. Optional `(mean, std)` normalisation comes from the sidecar's `norm_mean` / `norm_std`. | `model/tiny/dists_sq.onnx`, `model/tiny/nr_metric_v1.onnx` |
| 2 | `[N, F]` | Feature-vector model. The host materialises the `F` features (default canonical-6: `adm2`, `vif_scale0..3`, `motion2`) from libvmaf's classic feature collector at inference time. The sidecar's `feature_order` (or `features`) declares the slot-to-feature mapping; the sidecar's `feature_mean` / `feature_std` (or `input_mean` / `input_std`) apply a StandardScaler before the tensor is handed to ORT. | `model/tiny/fr_regressor_v1.onnx`, `model/tiny/fr_regressor_v2.onnx`, `model/tiny/vmaf_tiny_v4.onnx` |

The batch dimension `N` may be:

- the fixed value `1` (legacy single-sample exports), or
- the symbolic ONNX `dim_param` token (`'batch'`, `'N'`, …) which ORT
  reports back through the C API as `-1` — this is the default
  produced by `torch.onnx.export(..., dynamic_axes=...)` and is what
  every shipped NR checkpoint uses (ADR-0523).

A *fixed* batch greater than 1 is rejected: libvmaf's per-frame
inference loop feeds one sample per ORT Run call, so multi-sample
batches have no consumer today. The diagnostic reads
`tiny-model loader: <rank-4|feature-vector> model has fixed batch N;
only batch=1 or symbolic batch (-1) is supported`.

For rank-4 models, the spatial dims `H` and `W` must be known positive
values; symbolic H/W ("dynamic-resolution" exports) fails with
`tiny-model loader: rank-4 model has dynamic / non-positive spatial
dims (H=…, W=…); symbolic H/W is unsupported — re-export with a fixed
input resolution`. The scratch buffer is sized once at attach time, so
the runtime cannot accept varying resolution.

Anything other than rank 2 or 4 fails loud with a human-readable log
line: `tiny-model loader: model has input rank N, expected 2 (feature
vector) or 4 (NCHW image)`.

Rank-2 models may declare a **second** input — `fr_regressor_v2`, for
instance, takes a 14-dim `codec` block (one-hot encoder + preset_norm +
crf_norm). The loader discovers the second-input width via ORT and
allocates a zero-initialised scratch buffer pre-seeded to the
"unknown encoder" one-hot at the third-from-last slot. No public CLI /
C API exists today to populate the codec block with the real encoder
context; consumers needing codec-aware predictions should treat
`fr_regressor_v2` as approximate until that surface lands.

**ONNX external data is supported automatically.** Models shipped as
`<basename>.onnx` plus a sibling `<basename>.onnx.data` (the standard
ONNX-protobuf-external-data layout) load with no extra configuration —
ONNX Runtime resolves the sibling file when given the absolute model
path. The fork's `fr_regressor_v1` and `fr_regressor_v2` ship in this
layout.

## Surface 3 — ffmpeg filters

Apply `ffmpeg-patches/*.patch` against a pinned FFmpeg SHA (see
[`ffmpeg-patches/test/build-and-run.sh`](../../ffmpeg-patches/test/build-and-run.sh))
then:

```bash
# C1 / C2 scoring through vf_libvmaf.
ffmpeg -i ref.mp4 -i dis.mp4 \
    -lavfi "[0:v][1:v]libvmaf=tiny_model=/models/vmaf_tiny_fr_v1.onnx:tiny_device=cuda" \
    -f null -

# C3 learned pre-filter.
ffmpeg -i in.mp4 \
    -vf "vmaf_pre=model=/models/filter_denoise_residual_v1.onnx:device=cuda" \
    out.mp4
```

The `vmaf_pre` filter's `device=` option accepts the same twelve device
strings as `tiny_device=` in the `libvmaf` filter — `auto`, `cpu`, `cuda`,
`openvino`, `openvino-npu`, `openvino-cpu`, `openvino-gpu`, `coreml`,
`coreml-ane`, `coreml-gpu`, `coreml-cpu`, and `rocm` — all mapping to the
corresponding `VmafDnnDevice` enum values (ADR-0482).

## Execution-provider matrix

| Backend flag | ORT EP | Notes |
| --- | --- | --- |
| `--tiny-device cpu` | CPUExecutionProvider | Always available. |
| `--tiny-device cuda` | CUDAExecutionProvider | Requires CUDA-enabled ORT; shares context with libvmaf-cuda. |
| `--tiny-device openvino` | OpenVINOExecutionProvider | Covers Intel GPU / SYCL / oneAPI. Tries GPU device type first, falls back to CPU device type. Also covers the integrated Xe / Xe2 GPU on Intel AI-PC platforms (Meteor / Lunar / Arrow Lake) for free. |
| `--tiny-device openvino-npu` | OpenVINOExecutionProvider, `device_type=NPU` | Intel AI-PC NPU only (Meteor / Lunar / Arrow Lake). No fallback inside the explicit selector; if the EP isn't compiled in or no NPU silicon is present, the open downgrades to the CPU EP via the same two-stage `vmaf_ort_open()` fallback that all explicit-EP selectors share. End-to-end NPU validation pending hardware access — see [ADR-0332](../adr/0405-openvino-npu-ep-wiring.md) and [Research-0031](../research/0031-intel-ai-pc-applicability.md). |
| `--tiny-device openvino-cpu` | OpenVINOExecutionProvider, `device_type=CPU` | OpenVINO CPU plugin (skip the GPU.0 probe). Useful when you want OpenVINO's CPU implementation specifically — e.g. for parity testing against a measured `--tiny-device openvino-gpu` run, or as a stable fallback on hosts without Intel iGPU/NPU. |
| `--tiny-device openvino-gpu` | OpenVINOExecutionProvider, `device_type=GPU` | OpenVINO `GPU.0` plugin. Targets the iGPU / dGPU on systems where OpenVINO's `intel_gpu` plugin is the desired backend (Arc dGPU, Xe / Xe2 iGPU). |
| `--tiny-device coreml` | CoreMLExecutionProvider | Apple-only EP (macOS). CoreML auto-routes across the Apple Neural Engine (ANE), Metal-backed GPU, and CPU. The unscoped selector lets CoreML pick the compute unit per-op; use the explicit variants below to pin a single unit. See [ADR-0365](../adr/0365-coreml-ep-wiring.md). |
| `--tiny-device coreml-ane` | CoreMLExecutionProvider, `MLComputeUnits=CPUAndNeuralEngine` | Highest perf-per-watt on M-series silicon (M1, M2, M3, M4). Routes to the dedicated on-die Neural Engine and falls back to CPU for ops the ANE doesn't support. Recommended Apple-silicon entry point. |
| `--tiny-device coreml-gpu` | CoreMLExecutionProvider, `MLComputeUnits=CPUAndGPU` | Pins CoreML to Metal-backed GPU + CPU. Useful when a graph hits ANE op-coverage gaps and falls back to CPU more aggressively than expected. |
| `--tiny-device coreml-cpu` | CoreMLExecutionProvider, `MLComputeUnits=CPUOnly` | Universal CoreML CPU path. Functionally similar to the plain CPU EP but exercises the same dispatch shape as the other coreml-* variants — useful for diff-style debugging on macOS. |
| `--tiny-device rocm` | ROCmExecutionProvider | Requires ROCm-enabled ORT. |
| `--tiny-device auto` | best available | Ordered try-chain depends on platform: on macOS (`__APPLE__`), CoreML (auto-route across ANE/GPU/CPU) is probed first: CoreML → CUDA → OpenVINO:GPU → ROCm → CPU, targeting Apple Silicon immediately. On other platforms: CUDA → OpenVINO:GPU → ROCm → CoreML → CPU. NPU is **not** in the AUTO chain — opt-in only via `--tiny-device openvino-npu` because of NPU power-state latency floor on small graphs. |

### Graceful EP fallback

If the requested EP isn't compiled into the linked ORT build (for
example, you ask for `cuda` on a CPU-only ORT), the session still
opens — it silently degrades to the CPU EP rather than failing. This
matches `VmafDnnConfig.device` being documented as a *hint*, not a
requirement: a laptop and a workstation running the same binary get
the best EP each one has.

To see which EP actually bound, call
`vmaf_dnn_session_attached_ep()` on the session:

```c
VmafDnnSession *sess;
vmaf_dnn_session_open(&sess, "/models/m.onnx",
                      &(VmafDnnConfig){.device = VMAF_DNN_DEVICE_AUTO});
printf("bound EP: %s\n", vmaf_dnn_session_attached_ep(sess));
/* One of: "CPU", "CUDA", "OpenVINO:GPU", "OpenVINO:CPU", "OpenVINO:NPU", "ROCm", "CoreML", "CoreML:ANE", "CoreML:GPU", "CoreML:CPU" */
```

Consumers that need a hard failure on missing EP should assert on the
returned string at the call site (for example
`strcmp(ep, "CUDA") == 0`).

### fp16 I/O (`VmafDnnConfig.fp16_io`)

Setting `.fp16_io = true` enables a host-side fp32 ↔ fp16 round-trip
at the I/O boundary, triggered per input/output slot when the model's
graph declares that slot as `FLOAT16`. The public API always takes
fp32; libvmaf performs the cast internally. When the model declares
`FLOAT32` on a slot, `fp16_io = true` is a no-op at that slot. When
the EP is OpenVINO, the precision hint `FP16` is additionally passed
to the EP so intermediate compute also runs at half precision.

Example — running a FLOAT16-typed model:

```c
VmafDnnConfig cfg = {.device = VMAF_DNN_DEVICE_AUTO, .fp16_io = true};
VmafDnnSession *sess;
vmaf_dnn_session_open(&sess, "/models/m_fp16.onnx", &cfg);

float in[H*W] = { /* fp32 input */ };
float out[H*W];
VmafDnnInput  din = {.data = in,  .shape = (int64_t[4]){1,1,H,W}, .rank = 4};
VmafDnnOutput dout = {.data = out, .capacity = H*W};
vmaf_dnn_session_run(sess, &din, 1, &dout, 1);
```

## Expected cross-device variance

Running the same `.onnx` on two different EPs produces near-identical
scores:

- CPU vs CUDA (FP32): within **1e-4**.
- CPU vs CUDA (FP16 via `--tiny-fp16`): within **1e-2**.

CI exercises CPU-only. **No CI job checks tiny-AI cross-device parity today**,
so those two bounds are workstation measurements, not gated numbers.

The self-hosted-runner half of the picture is partly built:

- Two jobs in
  [`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml)
  target `runs-on: [self-hosted, linux, gpu-full]` — `Coverage GPU (advisory)`
  and `SYCL float_ssim Parity`. Setup is documented in
  [`docs/development/self-hosted-runner.md`](../development/self-hosted-runner.md).
- Both are additionally gated on the repository variable
  `GPU_COVERAGE_ENABLED == 'true'`, and on the PR not being a draft (ADR-0331).
- The variable is **not set** on `VMAFx/vmafx`, and the one registered runner
  carries the labels `self-hosted, Linux, X64, sycl-arc` — not `gpu-full`. So
  neither job can be scheduled even if the variable were flipped.
- Neither job would cover this page's claim anyway: they build CUDA/SYCL
  kernels and run the `float_ssim` parity test. Nothing runs a tiny-AI ONNX
  model on two execution providers and diffs the scores.

Until that changes, verify cross-device tiny-AI parity yourself before trusting
a GPU score — run the same `.onnx` under `--tiny-device cpu` and your target
device on the same pair and compare. `/cross-backend-diff` and
`scripts/ci/cross_backend_vif_diff.py` cover the *feature* backends, not the
tiny-AI execution providers.
