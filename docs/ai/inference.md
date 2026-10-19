<!-- markdownlint-disable MD060 -->
# Tiny AI — inference

Score a clip with a tiny ONNX model by passing it to `vmaf --tiny-model`. Three
consumer surfaces share one runtime
([`core/src/dnn/ort_backend.c`](../../core/src/dnn/ort_backend.c)): the `vmaf`
CLI, the libvmaf C API and the ffmpeg filters.

## Quick start

A full-reference tiny model reads libvmaf features, usually the canonical-6
(`adm2`, `vif_scale0..3`, `motion2`). Loading the model makes the run compute
the features its sidecar names, whatever classic model the run uses:

```bash
vmaf -r ref.yuv -d dis.yuv -w 1920 -h 1080 -p 420 -b 8 \
     --tiny-model model/tiny/vmaf_tiny_v2.onnx \
     --tiny-device cpu --json -o scores.json
```

The tiny score is a normal per-frame feature. Its name is the `name` field of
the model's sidecar JSON, or `vmaf_tiny_model` when the sidecar has no `name`
(as for `vmaf_tiny_v2`). It appears in `frames[].metrics` and in
`pooled_metrics`, next to the classic `vmaf` score:

```json
"pooled_metrics": {
  "vmaf_tiny_model": { "mean": 93.72, ... },
  "vmaf": { "mean": 94.32, ... }
}
```

How a feature-vector model gets its inputs
([ADR-1520](../adr/1520-tiny-model-feature-inputs-at-flush.md)):

- Loading the model registers the extractors that write its input features,
  with their default options. Those features appear in the output next to
  the tiny score. A sidecar that names a feature no extractor writes, or that
  names fewer or more features than the model's input is wide, makes
  `--tiny-model` fail.
- The model runs after the last frame, when every input is in. `motion2` of a
  frame is only known once the next frame has been read, so the tiny scores
  appear when the run is flushed, not while frames are read.
- A frame that lacks one of the inputs fails the run with
  `tiny model <name>: frame <n> has no value for input feature '<feature>'`.
  The model never reads a missing feature as 0.
- Frames that `--subsample` drops are not scored, as for the classic model.

Before this change the default `vmaf_v1.0.16_3d0h` run computed none of the
canonical-6, and `vmaf_tiny_v2` printed the same number (-0.853) for every
frame.

!!! note
    The shipped tiny models were trained against the `vmaf_v0.6.1` teacher. The
    one-shot retrain against `vmaf_v1.0.16_3d0h` is RC9 work, see the
    [roadmap](../roadmap.md).

## Prerequisites

- libvmaf built with `-Denable_dnn=enabled`, or `auto` with ONNX Runtime
  discoverable via `pkg-config` (`libonnxruntime` or `onnxruntime`).
- ONNX Runtime available at build time. It is not in the distro setup scripts
  under [scripts/setup/](../../scripts/setup/). Install the prebuilt release
  tarball from <https://github.com/microsoft/onnxruntime/releases> or a distro
  package, and put its `libonnxruntime.so` and headers on `PKG_CONFIG_PATH`
  before `meson setup`. The build files declare no minimum version.
- A `.onnx` model plus a sidecar `.json` pair, under `model/tiny/` or anywhere
  else. `--tiny-model` takes a file path, not a registry id.

Verify the build at runtime:

```bash
vmaf --help | grep -- '--tiny-model'   # must list the flag
vmaf --tiny-model /missing.onnx 2>&1   # should print a clear error,
                                       # not "option not found"
```

### Restrict where models load from

Set `VMAF_TINY_MODEL_DIR` to a trusted directory for deployment hardening. When
set, every ONNX path is resolved with symlinks followed and must live below
that directory before the loader stats or maps the file. Missing,
non-directory, sibling-prefix and symlink-escape paths fail closed with
`-EACCES`.

```bash
export VMAF_TINY_MODEL_DIR=/opt/vmaf-models
vmaf -r ref.yuv -d dis.yuv -w 1920 -h 1080 -p 420 -b 8 \
     --tiny-model /opt/vmaf-models/vmaf_tiny_v2.onnx
```

The jail is independent of `--tiny-model-verify`: the jail restricts where
models may load from, verification pins which signed model bytes may load.

## Surface 1: the `vmaf` CLI

### Examples

C1, full-reference, augmenting the classic SVM:

```bash
vmaf -r ref.yuv -d dis.yuv -w 1920 -h 1080 -p 420 -b 8 \
     --tiny-model model/tiny/vmaf_tiny_v2.onnx \
     --tiny-device cuda
```

C2, no-reference (the NR model needs `--tiny-resize` when the clip resolution
differs from the model input, see
[auto-resize](#auto-resize-for-image-input-models)):

```bash
vmaf -d dis.yuv -w 1920 -h 1080 -p 420 -b 8 \
     --tiny-model model/tiny/nr_metric_v1.onnx \
     --no-reference --tiny-resize bilinear
```

### Choose a VMAF-tiny model

The tiny VMAF fusion family shares the canonical-6 input contract
and the 0 to 100 output range. `vmaf_tiny_v2` is the recommended default
([ADR-0244](../adr/0244-vmaf-tiny-v2.md)); the v1 files stay on disk as
regression baselines.

| Model | Arch | Params | ONNX | NF LOSO PLCC | Use |
| --- | --- | ---: | ---: | ---: | --- |
| [`vmaf_tiny_v2`](models/vmaf_tiny_v2.md) | mlp_small | 257 | 2.5 KB | 0.9978 ± 0.0021 | Default; smallest bundle |
| [`vmaf_tiny_v3`](models/vmaf_tiny_v3.md) | mlp_medium | 769 | 4.5 KB | 0.9986 ± 0.0015 | Recommended higher tier; lowest-variance estimates |
| [`vmaf_tiny_v4`](models/vmaf_tiny_v4.md) | mlp_large | 3073 | 14.0 KB | 0.9987 ± 0.0015 | Top of the measured ladder |

v4's PLCC gain over v3 is +0.0001, below one standard deviation, so the ladder
saturates on the canonical-6 and 4-corpus regime (ADR-0242: the architecture
ladder stops here). Pick v3 unless you want the top rung, and v2 for the
smallest bundle.

### CLI flags

| Flag | Default | Notes |
| --- | --- | --- |
| `--tiny-model PATH` | none | ONNX model path (absolute or relative); sidecar JSON at `${PATH%.onnx}.json`. |
| `--tiny-device STR` | `auto` | One of the 12 device strings in the [EP matrix](#execution-provider-matrix). `--dnn-ep` is an alias that selects the ORT execution provider by its ORT name. |
| `--tiny-threads N` | `0` | CPU EP intra-op threads; 0 = ORT default. |
| `--tiny-fp16` | off | Request fp16 I/O when the EP supports it. |
| `--tiny-model-verify` | off | Require Sigstore-bundle verification (`cosign verify-blob`) before model load. Refuses to load on a missing bundle, missing `cosign`, or non-zero exit. See [model-registry.md](model-registry.md) and [security.md](security.md). |
| `--tiny-codec NAME` | none | Encoder of the distorted clip; required by codec-aware models (`fr_regressor_v2`, `fr_regressor_v3`). Must be an entry of the model sidecar's `encoder_vocab`. See [codec-aware models](#codec-aware-models). |
| `--tiny-preset STR` | `medium` | Encoder preset (`medium`, `slow`, `p4`, `5`, ...). Encoder-specific; mirrors `ai/scripts/train_fr_regressor_v2.py::PRESET_ORDINAL`. Unknown presets fall back to ordinal 5. A model trained with one preset value (`fr_regressor_v3`) ignores it and warns. |
| `--tiny-crf N` | none | CRF or QP integer used during encoding, normalised as the model's sidecar declares (see [codec-aware models](#codec-aware-models)). Required with `--tiny-codec` or `--tiny-preset`. |
| `--tiny-resize MODE` | `disabled` | Auto-resize for fixed-shape image models: `disabled`, `bilinear`, `nearest`, `bicubic`. |
| `--no-reference` | off | Skip reference loading; valid only with an NR tiny model. |

### Codec-aware models

`fr_regressor_v2.onnx` carries a second `codec` input of shape
`[batch, N_VOCAB + 2]`:

- The first `N_VOCAB` slots are a one-hot over the sidecar's `encoder_vocab`.
  For v2 that is 12 entries: `libx264`, `libx265`, `libsvtav1`, `libvvenc`,
  `libvpx-vp9`, `h264_nvenc`, `hevc_nvenc`, `av1_nvenc`, `h264_qsv`,
  `hevc_qsv`, `av1_qsv`, `unknown`.
- The last two slots are `preset_norm` and `crf_norm`, normalised as the
  model's trainer did. For `fr_regressor_v2` (and any sidecar without the
  keys below) that is `preset_ordinal / 9.0` and `crf / 63.0`. A sidecar can
  declare another encoding
  ([ADR-1558](../adr/1558-codec-block-encoding-from-sidecar.md)):
  `codec_preset_norm` `ordinal` or `constant` (with `codec_preset_value`), and
  `codec_crf_norm` `div63` or `minmax` (with `codec_crf_min` and
  `codec_crf_max`). `fr_regressor_v3` uses a constant 0.5 and CRF min-max over
  19..37. An unknown value refuses the model at load.

A codec-aware model needs `--tiny-codec` (and the encode's `--tiny-preset`
and `--tiny-crf`). Without it the run stops on the first frame with
`tiny model <name> is codec-aware: name the codec ...`
([ADR-1520](../adr/1520-tiny-model-feature-inputs-at-flush.md)); an earlier
loader filled the block with a guess instead. Pass `--tiny-codec unknown` when
the encoder is not known and the model's vocabulary has an `unknown` entry
(`fr_regressor_v2` has one, `fr_regressor_v3` does not). The flags fill the
block through the public `vmaf_dnn_set_codec_context()` API (ADR-0519):

```bash
vmaf --reference src.yuv --distorted dst.yuv \
     --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
     --tiny-model model/tiny/fr_regressor_v2.onnx \
     --tiny-codec libx264 --tiny-preset medium --tiny-crf 28 \
     --json --output /tmp/scores.json
```

The flags are validated at attach time:

- A codec name that is not in the sidecar's `encoder_vocab` exits non-zero
  before the first frame is read. Common ffprobe aliases (`h264`, `hevc`,
  `av1`, `vp9`, `vvc`) are accepted. See
  [ADR-0522](../adr/0522-tiny-codec-preset-crf-cli-flags.md).
- A model without a codec block (`fr_regressor_v1`, `vmaf_tiny_v4`,
  `dists_sq`) rejects the flags with a `-ENOTSUP` message.
- `--tiny-codec` or `--tiny-preset` without `--tiny-crf` exits non-zero: the
  CRF is a model input, and the CLI does not make one up.

```text
$ vmaf … --tiny-codec UNKNOWN_ENC …
--tiny-codec 'UNKNOWN_ENC' not found in model encoder_vocab;
use one of the names in the model sidecar's encoder_vocab.
```

### Multi-output models

For attached multi-output models, each scalar ONNX output is its own feature:

- A single-output model keeps the sidecar `name` as the score key.
- A multi-output model uses `<sidecar-name>_<output-name>`. `output-name` comes
  from the sidecar `output_names[]` when present and count-matched, otherwise
  from the ONNX graph output name.
- Attached mode rejects non-scalar output tensors. Use
  `vmaf_dnn_session_run()` when the caller needs vectors or images.

### Auto-resize for image-input models

Image-input (rank-4 NCHW, where N is batch, C channels, H height, W width) tiny
models declare a fixed input shape. The shipped `model/tiny/nr_metric_v1.onnx`
NR scorer expects `[1, 1, 224, 224]` because it was trained on KoNViD-1k
middle-frames downscaled to 224x224 grayscale. Most NR workflows pass the
encoder's native resolution as `--width` and `--height`, so a dimension
mismatch is the norm.

The per-frame dispatch can auto-resample the luma plane to the model input
shape (ADR-0550). The default is `disabled`: a mismatch returns `-ERANGE` and
you must choose a filter, which keeps strict mode for parity harnesses and
avoids a silent free parameter.

| `--tiny-resize` | Filter |
| --- | --- |
| `disabled` | Default. Mismatch returns `-ERANGE`. |
| `bilinear` | torchvision / OpenCV BILINEAR (half-pixel-centre). |
| `nearest` | Nearest-neighbour, floor coordinate (debug-friendly). |
| `bicubic` | Catmull-Rom (a = -0.5), separable. |

!!! warning
    `bilinear`, `nearest` and `bicubic` produce scores that differ by about 2%
    on the same input. Treat the filter as a model hyperparameter and document
    it alongside the model checkpoint.

When the source dimensions already equal the model dimensions, the dispatch
forwards verbatim to `vmaf_tensor_from_luma`. The matched-dimension path stays
bit-identical to the pre-ADR-0550 code, so the Netflix golden gate is
unaffected by the selected filter. The same selector is reachable from the C
API as `vmaf_dnn_set_resize_mode(ctx, VMAF_DNN_RESIZE_BILINEAR | _NEAREST |
_BICUBIC | _DISABLED)`.

Smoke test with an explicit filter:

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

Without `--tiny-resize`, the default produces 0 frames and a "problem reading
pictures" error at frame 0 for any size-mismatched NR model:

```bash
vmaf --no-reference \
     --tiny-model model/tiny/nr_metric_v1.onnx \
     --distorted testdata/dis_576x324_48f.yuv \
     --width 576 --height 324 --pixel_format 420 --bitdepth 8
```

## Surface 2: the libvmaf C API

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
int err = vmaf_use_tiny_model(ctx, "/models/vmaf_tiny_v2.onnx", &dnn_cfg);
if (err < 0) { /* handle -errno */ }

/* … feed frames as usual; tiny-model scores appear in the same
     per-frame collector the built-in SVM uses. */
```

The sidecar JSON is discovered automatically at `${onnx_path%.onnx}.json`. Its
`kind` field (`fr` or `nr`) tells libvmaf whether to expect a reference.
Optional `output_names[]` entries name attached multi-output scalar scores; the
legacy `output_name` field stays accepted for single-output metadata.

### Codec context functions

| Function | Purpose | Notable returns |
| --- | --- | --- |
| `vmaf_dnn_set_codec_context(ctx, codec_name, preset, crf)` | Fill the codec block of a codec-aware model; the model does not score until this succeeds. Call before the first `vmaf_read_pictures()`; not thread-safe. NULL or `""` codec maps to the vocabulary's `unknown` entry. | `0` ok; `-ENOENT` codec not in `encoder_vocab`, or NULL / `""` with no `unknown` entry (the model will not score); `-ENOTSUP` model has no codec block; `-EINVAL` no model attached; `-ENOSYS` built without DNN |
| `vmaf_dnn_is_codec_aware(ctx)` | `1` when the attached model needs a codec context, else `0`. Safe with a NULL context. | `0` or `1` |

### Accepted ONNX input shapes

The loader accepts two input ranks (ADR-0518, extended by ADR-0523):

| Rank | Shape | Meaning | Example checkpoint |
| --- | --- | --- | --- |
| 4 | `[N, 1, H, W]` | NCHW single-channel luma image. The picture's Y plane is fed through `vmaf_tensor_from_luma` each frame. Optional `(mean, std)` normalisation comes from the sidecar's `norm_mean` / `norm_std`. | `model/tiny/dists_sq.onnx`, `model/tiny/nr_metric_v1.onnx` |
| 2 | `[N, F]` | Feature-vector model. The sidecar's `feature_order` (or `features`) names the feature of every slot; a six-wide model without that list reads the canonical-6. Attaching registers the extractors of those features, and the model runs at flush on the values the run computed. `feature_mean` / `feature_std` (or `input_mean` / `input_std`) apply a StandardScaler before the tensor reaches ORT unless the graph carries it (`onnx_has_scaler`). | `model/tiny/fr_regressor_v1.onnx`, `model/tiny/fr_regressor_v2.onnx`, `model/tiny/vmaf_tiny_v4.onnx` |

The batch dimension `N` may be:

- the fixed value `1` (legacy single-sample exports), or
- a symbolic ONNX `dim_param` token (`'batch'`, `'N'`, ...), which ORT reports
  through the C API as `-1`. This is the default of
  `torch.onnx.export(..., dynamic_axes=...)` and what every shipped NR
  checkpoint uses (ADR-0523).

Anything else is rejected with a log line:

| Condition | Diagnostic |
| --- | --- |
| Fixed batch greater than 1 (libvmaf feeds one sample per ORT Run call) | `tiny-model loader: <rank-4\|feature-vector> model has fixed batch N; only batch=1 or symbolic batch (-1) is supported` |
| Rank-4 model with symbolic or non-positive H or W (the scratch buffer is sized once at attach time) | `tiny-model loader: rank-4 model has dynamic / non-positive spatial dims (H=…, W=…); symbolic H/W is unsupported — re-export with a fixed input resolution` |
| Input rank other than 2 or 4 | `tiny-model loader: model has input rank N, expected 2 (feature vector) or 4 (NCHW image)` |
| Sidecar feature list of another length than the feature input | `tiny-model loader: sidecar lists N input features but the model's feature input has M slots` |
| Feature-vector model not six wide and without a sidecar feature list | `tiny-model loader: feature input slot I of N has no feature name; ...` |
| Sidecar names a feature no extractor writes | `tiny-model loader: input feature '<name>' (slot I) is written by no feature extractor` |
| Second input without a sidecar `encoder_vocab`, or of another width than the vocabulary plus two | `tiny-model loader: model has a second input of width N but its sidecar declares no encoder_vocab, ...` / `tiny-model loader: second input has width N but the sidecar's encoder_vocab (V entries) describes a codec block of width V+2` |

Rank-2 models may declare a second input. `fr_regressor_v2`, for instance,
takes a 14-dim `codec` block (one-hot encoder, `preset_norm`, `crf_norm`). The
loader refuses a second input unless the sidecar's `encoder_vocab` has exactly
two entries fewer than the input is wide. The block starts empty and the model
does not score until `--tiny-codec`, `--tiny-preset` and `--tiny-crf` or
`vmaf_dnn_set_codec_context()` fill it, see
[codec-aware models](#codec-aware-models).

!!! note
    ONNX external data is supported automatically. A model shipped as
    `<basename>.onnx` plus a sibling `<basename>.onnx.data` (the standard
    external-data layout) loads with no extra configuration, because ONNX
    Runtime resolves the sibling file from the absolute model path.
    `fr_regressor_v1` and `fr_regressor_v2` ship this way.

## Surface 3: ffmpeg filters

Apply `ffmpeg-patches/*.patch` against the FFmpeg release named by
`FFMPEG_TAG` in `build-config.env` (the series is listed in
[`ffmpeg-patches/series.txt`](../../ffmpeg-patches/series.txt); the harness is
[`ffmpeg-patches/test/build-and-run.sh`](../../ffmpeg-patches/test/build-and-run.sh)).
Then:

```bash
# C1 / C2 scoring through vf_libvmaf.
ffmpeg -i dis.mp4 -i ref.mp4 \
    -lavfi "[0:v][1:v]libvmaf=tiny_model=/models/vmaf_tiny_v2.onnx:tiny_device=cuda" \
    -f null -

# C3 learned pre-filter.
ffmpeg -i in.mp4 \
    -vf "vmaf_pre=model=/models/learned_filter_v1.onnx:device=cuda" \
    out.mp4
```

| Filter | Options |
| --- | --- |
| `libvmaf` | `tiny_model`, `tiny_device` (default `auto`), `tiny_threads` (default 0), `tiny_fp16` (default 0) |
| `vmaf_pre` | `model`, `device`, `threads`, `chroma` (0 or 1; default 0 = luma only) |

The `vmaf_pre` `device=` option accepts the same twelve device strings as
`tiny_device=`, all mapping to the `VmafDnnDevice` enum (ADR-0482).

## Execution-provider matrix

`--tiny-device` selects the ONNX Runtime execution provider (EP).

| Device | ORT EP | Notes |
| --- | --- | --- |
| `cpu` | CPUExecutionProvider | Always available. |
| `cuda` | CUDAExecutionProvider | Needs CUDA-enabled ORT; shares the context with libvmaf-cuda. |
| `openvino` | OpenVINOExecutionProvider | Intel GPU / SYCL / oneAPI, including the integrated Xe / Xe2 GPU of Meteor, Lunar and Arrow Lake. Tries the GPU device type first, then CPU. |
| `openvino-npu` | OpenVINOExecutionProvider, `device_type=NPU` | Intel AI-PC NPU only. See the warning below. |
| `openvino-cpu` | OpenVINOExecutionProvider, `device_type=CPU` | OpenVINO CPU plugin; skips the GPU.0 probe. For parity tests against `openvino-gpu`, or as a stable fallback without Intel iGPU/NPU. |
| `openvino-gpu` | OpenVINOExecutionProvider, `device_type=GPU` | OpenVINO `GPU.0` plugin (Arc dGPU, Xe / Xe2 iGPU). |
| `coreml` | CoreMLExecutionProvider | macOS only. CoreML picks the compute unit per op across the Neural Engine (ANE), Metal GPU and CPU ([ADR-0365](../adr/0365-coreml-ep-wiring.md)). |
| `coreml-ane` | CoreML, `MLComputeUnits=CPUAndNeuralEngine` | Best performance per watt on M-series. Falls back to CPU for ops the ANE lacks. Recommended Apple-silicon entry point. |
| `coreml-gpu` | CoreML, `MLComputeUnits=CPUAndGPU` | Pins Metal GPU plus CPU; useful when ANE op gaps force CPU fallback. |
| `coreml-cpu` | CoreML, `MLComputeUnits=CPUOnly` | CoreML CPU path; same dispatch shape as the other `coreml-*` variants, for diff-style debugging. |
| `rocm` | ROCmExecutionProvider | Needs ROCm-enabled ORT. |
| `auto` | best available | Ordered try-chain, see below. |

!!! warning
    `openvino-npu` has no fallback inside its own selector. If the EP is not
    compiled in or no NPU is present, the open downgrades to the CPU EP through
    the two-stage `vmaf_ort_open()` fallback shared by all explicit-EP
    selectors. End-to-end NPU validation is pending hardware access, see
    [ADR-0332](../adr/0405-openvino-npu-ep-wiring.md) and
    [Research-0031](../research/0031-intel-ai-pc-applicability.md).

### `auto` order

| Platform | Try-chain |
| --- | --- |
| macOS (`__APPLE__`) | CoreML, CUDA, OpenVINO:GPU, ROCm, CPU |
| Other | CUDA, OpenVINO:GPU, ROCm, CoreML, CPU |

The NPU is not in the `auto` chain. It is opt-in through
`--tiny-device openvino-npu` because of the NPU power-state latency floor on
small graphs.

### Graceful EP fallback

If the requested EP is not compiled into the linked ORT build (for example
`cuda` on a CPU-only ORT), the session still opens and degrades to the CPU EP
instead of failing. `VmafDnnConfig.device` is a hint, not a requirement, so a
laptop and a workstation running the same binary get the best EP each has.

The same holds one step later. An EP can register successfully yet fail when
ONNX Runtime creates the session on it, usually a CUDA-enabled ONNX Runtime on a
machine without an NVIDIA GPU. The session is then recreated on the CPU EP
([ADR-0113](../adr/0113-ort-create-session-fallback-multi-ep-ci.md)). Both
fallbacks are expected, so they log at `DEBUG`. A session that cannot be created
on the CPU EP either still logs a `WARNING` and fails.

To see which EP bound, call `vmaf_dnn_session_attached_ep()`:

```c
VmafDnnSession *sess;
vmaf_dnn_session_open(&sess, "/models/m.onnx",
                      &(VmafDnnConfig){.device = VMAF_DNN_DEVICE_AUTO});
printf("bound EP: %s\n", vmaf_dnn_session_attached_ep(sess));
/* One of: "CPU", "CUDA", "OpenVINO:GPU", "OpenVINO:CPU", "OpenVINO:NPU", "ROCm", "CoreML", "CoreML:ANE", "CoreML:GPU", "CoreML:CPU" */
```

Consumers that need a hard failure on a missing EP should assert on the
returned string, for example `strcmp(ep, "CUDA") == 0`.

### fp16 I/O

`VmafDnnConfig.fp16_io = true` enables a host-side fp32 to fp16 round trip at
the I/O boundary:

- It triggers per input or output slot when the model's graph declares that slot
  as `FLOAT16`. The public API always takes fp32; libvmaf casts internally.
- On a slot the model declares `FLOAT32`, `fp16_io = true` is a no-op.
- With the OpenVINO EP, the precision hint `FP16` is also passed to the EP, so
  intermediate compute runs at half precision.

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

The same `.onnx` on two different EPs produces near-identical scores:

| Pair | Agreement |
| --- | --- |
| CPU vs CUDA (FP32) | within 1e-4 |
| CPU vs CUDA (FP16 via `--tiny-fp16`) | within 1e-2 |

No CI job runs a second execution provider yet, so those two bounds are
workstation measurements until a runner with a CUDA-enabled ONNX Runtime runs the
gate. The gate exists and fails closed:

```bash
python3 scripts/ci/tiny_ai_cross_device_parity_gate.py \
  --reference-provider CPUExecutionProvider --target-provider CUDAExecutionProvider \
  --json-out parity.json --md-out parity.md
```

It exits 1 and names the cause when the target provider is not installed
(`MISSING_PROVIDER`), when ONNX Runtime accepted it but ran the session on the
CPU (`PROVIDER_UNBOUND`), when the feature fixture is missing (`BAD_FIXTURE`), or
when an output differs by more than the bound (`FAIL`). `--allow-missing-provider`
is for inspection only. `vmaf-train cross-backend --fail-on-mismatch` is
fail-closed the same way: a run that compared nothing, or whose provider fell
back to the CPU, is not ok. The self-hosted-runner lanes are separate and do not
run it yet:

- [`sycl-parity.yml`](../../.github/workflows/sycl-parity.yml) owns Arc-only
  feature parity behind `SYCL_ARC_RUNNER_ENABLED` and the `sycl-arc` label.
- [`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml)
  owns the combined CUDA + SYCL `Coverage GPU` job behind
  `GPU_COVERAGE_ENABLED` and the `gpu-full` label. A hosted live-runner probe
  prevents it from queuing on an impossible label set (ADR-1319).
- As of 2026-09-25, the repository and organisation APIs return zero registered
  runners and the repository has zero Actions variables. Both hardware lanes
  are therefore disabled and neither is current hardware evidence.
- Nothing runs the same ONNX model on two execution providers and diffs the
  scores.

Until that changes, verify cross-device parity yourself before trusting a GPU
score: run the same `.onnx` under `--tiny-device cpu` and under your target
device on the same pair, and compare. `/cross-backend-diff` and
`scripts/ci/cross_backend_vif_diff.py` cover the feature backends, not the
tiny-AI execution providers.

## History

- **2026-05-02, ADR-0241.** `vmaf_tiny_v3` (`mlp_medium`, 6 to 32 to 16 to 1,
  about 769 parameters) became available alongside v2, trained on the same
  4-corpus parquet with the same recipe. Netflix LOSO mean PLCC 0.9986 ± 0.0015
  against v2's 0.9978 ± 0.0021 (+0.0008 mean, -30 % std). v2 remained the
  production default.
- **2026-04-29.** `vmaf_tiny_v2` replaced `vmaf_tiny_v1` as the recommended tiny
  FR fusion model: same canonical-6 input contract, same 0 to 100 output
  range, +0.005 to +0.018 PLCC across the Phase-3 validation chain. The v1 file
  stays on disk as a regression baseline, see
  [`models/vmaf_tiny_v2.md`](models/vmaf_tiny_v2.md).
