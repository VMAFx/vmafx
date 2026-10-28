<!-- markdownlint-disable MD013 MD060 -->
# DNN session API — `libvmaf/dnn.h`

!!! warning "Deprecated: `libvmaf.h` compatibility API"
    This page documents the compatibility library `libvmaf.so.3`. New code
    uses the [VMAFx API](vmafx/index.md); the [migration table](vmafx/compat.md)
    names the successor of every function.

The DNN surface in
[`core/include/libvmaf/dnn.h`](../../core/include/libvmaf/dnn.h)
lets callers load and run tiny ONNX models from C, either attached to a
`VmafContext` (so DNN scores show up next to SVM scores in the normal VMAF
report) or as a standalone session (luma-in / luma-out filter-style
inference, no VmafContext required).

This is the runtime half of the tiny-AI surface; training lives in `ai/`.
See [ADR-0022](../adr/0022-inference-runtime-onnx.md) (ORT as the inference
runtime) and [ADR-0023](../adr/0023-tinyai-user-surfaces.md) (the four user
surfaces: CLI, C API, ffmpeg, training).

## Availability check

```c
int vmaf_dnn_available(void);
```

Returns `1` if libvmaf was built with DNN support (the `enable_dnn` Meson
feature option, `enabled` or `auto` with ONNX Runtime found) and ONNX Runtime
is linked, `0` otherwise. It is the cheap way to branch between DNN and
classic-only builds at run time. It is safe to call from any thread.

When it returns `0`, the other entry points in `dnn.h` return `-ENOSYS`
(except `vmaf_dnn_is_codec_aware()`, which returns `0`, and
`vmaf_dnn_session_attached_ep()`, which returns `NULL`). The header is
installed only when `enable_dnn` is `enabled` or `auto`.

All entry points are not thread-safe unless stated; see
[thread-safety](#thread-safety).

## Device config — `VmafDnnConfig`

```c
typedef enum VmafDnnDevice {
    VMAF_DNN_DEVICE_AUTO     = 0,
    VMAF_DNN_DEVICE_CPU      = 1,
    VMAF_DNN_DEVICE_CUDA     = 2,
    VMAF_DNN_DEVICE_OPENVINO = 3,  /* OpenVINO GPU with CPU fallback */
    VMAF_DNN_DEVICE_ROCM     = 4,
    VMAF_DNN_DEVICE_COREML   = 5,
    VMAF_DNN_DEVICE_COREML_ANE = 6,
    VMAF_DNN_DEVICE_COREML_GPU = 7,
    VMAF_DNN_DEVICE_COREML_CPU = 8,
    VMAF_DNN_DEVICE_OPENVINO_NPU = 9,
    VMAF_DNN_DEVICE_OPENVINO_CPU = 10,
    VMAF_DNN_DEVICE_OPENVINO_GPU = 11,
} VmafDnnDevice;

typedef struct VmafDnnConfig {
    VmafDnnDevice device;
    int  device_index;  /* multi-GPU index; 0 for single-GPU/CPU */
    int  threads;       /* CPU EP intra-op threads; 0 = ORT default */
    bool fp16_io;       /* request fp16 tensors when supported */
} VmafDnnConfig;
```

| `device` | Execution provider (EP) tried | Fallback |
| --- | --- | --- |
| `AUTO` | CUDA, then OpenVINO GPU, then ROCm, then CoreML | CPU. OpenVINO NPU is explicit-only: small graphs can pay a noticeable NPU power-state latency floor. |
| `CPU` | ORT CPU EP | none |
| `CUDA` | `CUDAExecutionProvider`, when the linked ORT exports it | CPU if the EP append fails; the session still opens |
| `OPENVINO` | OpenVINO `device_type=GPU` | OpenVINO `device_type=CPU`, then CPU |
| `OPENVINO_NPU`, `_CPU`, `_GPU` | OpenVINO pinned to one `device_type` (`NPU`, `CPU`, `GPU`), no OpenVINO fallback | CPU when the EP or the silicon is missing |
| `ROCM` | `ROCMExecutionProvider` | CPU |
| `COREML` | `CoreMLExecutionProvider`, CoreML chooses compute units (`ALL`) | CPU on non-Apple ORT builds |
| `COREML_ANE` | CoreML with `MLComputeUnits` = `CPUAndNeuralEngine` | CPU on non-Apple ORT builds |
| `COREML_GPU` | CoreML with `CPUAndGPU` | CPU on non-Apple ORT builds |
| `COREML_CPU` | CoreML with `CPUOnly` | CPU on non-Apple ORT builds |

Other fields:

- `device_index`: multi-GPU index; 0 for a single GPU or CPU.
- `threads`: CPU EP intra-op threads. `0` lets ORT pick; set it explicitly
  when pinning affinity or benchmarking.
- `fp16_io`: stages fp32 to fp16 for model slots declared `FLOAT16`;
  OpenVINO also receives `precision=FP16`. It does not change float32
  slots, so it is not a speed switch for fp32-only graphs.

EP choice is a preference, not a requirement: when a requested provider is
missing from the linked ORT build, session open falls back to CPU. Check
[which EP bound](#which-execution-provider-bound-vmaf_dnn_session_attached_ep)
if your application must fail instead.

Pass `NULL` for `cfg` in any function that accepts one to use
`VMAF_DNN_DEVICE_AUTO` with zero device index, zero threads, and no fp16 I/O.

## Attached mode — `vmaf_use_tiny_model`

```c
int vmaf_use_tiny_model(VmafContext *ctx,
                        const char *onnx_path,
                        const VmafDnnConfig *cfg);
```

Register a tiny ONNX model on a live `VmafContext`. The model participates in
the per-frame pipeline; its outputs appear in the report alongside SVM
scores. Use this when you want "VMAF + tiny AI score" in the same run.

A feature-vector model (rank-2 input) reads libvmaf features
([ADR-1520](../adr/1520-tiny-model-feature-inputs-at-flush.md)):

- Attaching registers the extractors that write the features its sidecar
  names (`feature_order` or `features`; canonical-6 for a six-wide model
  without a sidecar list), with their default options.
- Its scores are computed when the context is flushed
  (`vmaf_read_pictures(ctx, NULL, NULL, 0)`), so read them after the flush.
- A frame that lacks one of the inputs makes the flush return `-EINVAL`, and
  the log names the feature and the frame. Frames before it keep their
  scores; frames `n_subsample` drops are not scored.

An image model (rank-4 input) is scored as each frame is read.

Returns:

- `0` — success.
- `-ENOSYS` — built without DNN support.
- `-EINVAL` — bad args (null `ctx` or `onnx_path`).
- `-ENOENT` — `onnx_path` does not exist or is not a regular file.
- `-E2BIG` — file exceeds the compile-time 50 MB cap
  (`VMAF_DNN_DEFAULT_MAX_BYTES` — defence against adversarial bloat;
  see [ADR-0039](../adr/0039-onnx-runtime-op-walk-registry.md)). The
  historical `VMAF_MAX_MODEL_BYTES` env override was retired in T7-12.
- `-ENOMEM` — session allocation failed (ORT env, session options, or
  internal buffer allocation).
- Negative `errno` from the operator-allowlist walk if the model contains a
  banned op.
- `-ENOTSUP` — a feature-vector model whose sidecar names another number of
  features than the input is wide, or whose second input does not match the
  sidecar's `encoder_vocab`.
- `-EINVAL` — a feature-vector model whose sidecar names a feature no
  extractor writes.

Equivalent CLI flag: `--tiny-model <path>`
([usage/cli.md](../usage/cli.md#tiny-ai-flags)).

Attached scores are written to the normal feature collector:

- A single-output model preserves the historical key: the sidecar `name` field
  (or `vmaf_tiny_model` when no sidecar name exists).
- A multi-output model emits one scalar score per graph output. The key is
  `<sidecar-name>_<output-name>`, where `output-name` comes from sidecar
  `output_names[]` when the array length matches the ONNX output count, or from
  the ONNX graph output name otherwise. The suffix is sanitized to
  `[A-Za-z0-9_]`; duplicate sanitized suffixes fall back to deterministic
  `output<slot>_<attempt>` keys.

## Codec-aware tiny-model inputs — `vmaf_dnn_set_codec_context`

Codec-conditioned tiny models (e.g. the v2 ladder regressor) accept a
small categorical block alongside the per-frame features: encoder
identity, preset ordinal, and CRF / QP. `vmaf_dnn_set_codec_context`
populates that block on the attached tiny model so the loop body does
not need to re-supply it per frame.

!!! warning
    Call it after `vmaf_use_tiny_model()` and before the first
    `vmaf_read_pictures()`. A codec-aware model does not score until this
    call has returned `0`: `vmaf_read_pictures()` returns `-EINVAL` for it
    and logs `tiny model <name> is codec-aware: ...`
    ([ADR-1520](../adr/1520-tiny-model-feature-inputs-at-flush.md)).

```c
int vmaf_dnn_set_codec_context(VmafContext *ctx,
                               const char *codec_name,
                               const char *preset,
                               int crf);
```

### Parameters

| Parameter    | Notes                                                                                                                                  |
|--------------|----------------------------------------------------------------------------------------------------------------------------------------|
| `ctx`        | Context with a tiny model already attached via `vmaf_use_tiny_model()`.                                                                |
| `codec_name` | Encoder name (`libx264`, `libx265`, `libsvtav1`, `libvpx-vp9`, `h264_nvenc`, ...). `NULL` or `""` selects the vocabulary's `"unknown"` entry; a vocabulary without one (`fr_regressor_v3`) returns `-ENOENT`. ffprobe aliases (`h264`, `hevc`, `av1`, `vp9`, `vvc`) map to their canonical encoder names. |
| `preset`     | Preset string (`medium`, `slow`, `p4`, `5`, ...), looked up in a per-encoder ordinal table. `NULL` or an unknown preset defaults to ordinal 5 (mid-tier).   |
| `crf`        | CRF / QP integer; clamped to `[0, 63]`.                                                                                                |

### Returns

| Code        | Meaning                                                                                            |
|-------------|----------------------------------------------------------------------------------------------------|
| `0`         | Codec block written (a named encoder, or the vocabulary's `"unknown"` entry for `NULL` / `""`).    |
| `-ENOENT`   | `codec_name` is not in the model's `encoder_vocab`, or is `NULL` / `""` and the vocabulary has no `"unknown"` entry. The model will not score. |
| `-ENOSYS`   | libvmaf was built without DNN support (`-Denable_dnn=disabled`).                                   |
| `-EINVAL`   | `ctx` is `NULL` or no tiny model is attached.                                                      |
| `-ENOTSUP`  | The attached model has no codec block (rank-4 image model or rank-2 single-input model).           |

Equivalent CLI flags (the `vmaf` CLI calls this internally):
`--tiny-codec`, `--tiny-preset`, `--tiny-crf` — see
[usage/cli.md](../usage/cli.md#codec-context-flags).
Per-codec / per-preset vocabularies live in the model's sidecar JSON
under `encoder_vocab` and `preset_vocab`; the loader bakes them into
the runtime descriptor at `vmaf_use_tiny_model()` time. The block layout
is `[encoder_onehot(N_VOCAB), preset_norm, crf_norm]`.

### Detect a codec-aware model

```c
int vmaf_dnn_is_codec_aware(const VmafContext *ctx);
```

Returns `1` when the tiny model attached to `ctx` requires a codec context
(its sidecar declares `"codec_aware": true` and it was loaded with a
`codec_block` second input). Returns `0` when no model is attached, the
model has no codec block, `ctx` is `NULL`, or libvmaf was built without DNN.
Call it after `vmaf_use_tiny_model()` to emit a clear error before inference
when `--tiny-codec` style conditioning is required. It is safe to call
before `vmaf_read_pictures()`.

## Tiny-model auto-resize — `vmaf_dnn_set_resize_mode`

NCHW tiny models declare a fixed input shape at training time (e.g.
224×224 for the `nr_metric_v1` NR scorer). When the user-supplied frame
dims don't match, the per-frame dispatch resamples the luma plane to
the model dims using the selected filter before invoking ONNX Runtime.
Bit-exact when source dims already equal model dims (the routine
forwards to `vmaf_tensor_from_luma` unchanged). Introduced in
[ADR-0550](../adr/0550-tiny-model-auto-resize.md).

```c
typedef enum VmafDnnResizeMode {
    VMAF_DNN_RESIZE_DISABLED = 0, /* default; mismatch → -ERANGE      */
    VMAF_DNN_RESIZE_BILINEAR = 1, /* OpenCV INTER_LINEAR / torchvision */
    VMAF_DNN_RESIZE_NEAREST  = 2, /* nearest, floor coord              */
    VMAF_DNN_RESIZE_BICUBIC  = 3, /* Catmull-Rom (a = -0.5)            */
} VmafDnnResizeMode;

int vmaf_dnn_set_resize_mode(VmafContext *ctx, VmafDnnResizeMode mode);
```

### Filter semantics

| Mode       | Equivalent                                                              | When to use                                            |
|------------|-------------------------------------------------------------------------|--------------------------------------------------------|
| `DISABLED` | None — size mismatch returns `-ERANGE`                                  | Parity harnesses; strict-mode pipelines (default).     |
| `BILINEAR` | torchvision `Resize(..., antialias=False)` / OpenCV `INTER_LINEAR`      | Every shipped NR / image-input tiny-AI model was trained against this.       |
| `NEAREST`  | OpenCV `INTER_NEAREST`; deterministic floor of source coord             | Cheaper; debugging dispatch without a filter parameter.                      |
| `BICUBIC`  | Separable Catmull-Rom (`a = -0.5`); torchvision `BICUBIC`               | Parity with exporters that used `transforms.Resize(interpolation=BICUBIC)`.  |

The three filter modes produce scores that differ by approximately 2%
on the same input — treat filter choice as a model hyperparameter and
document it alongside the model checkpoint.

### Resize-mode returns

| Code      | Meaning                                                              |
|-----------|----------------------------------------------------------------------|
| `0`       | Resize mode updated; takes effect on the next `vmaf_read_pictures()`. |
| `-EINVAL` | `ctx` is `NULL`, or `mode` is outside the enum range.                |
| `-ENOSYS` | libvmaf was built without DNN support.                               |

Equivalent CLI flag: `--tiny-resize <bilinear|nearest|bicubic|disabled>`
— see [usage/cli.md](../usage/cli.md#codec-context-flags).
May be called before or after `vmaf_use_tiny_model()`; the setting is
sticky for the lifetime of the context.

## Standalone sessions — `VmafDnnSession`

Standalone mode is for filter-style inference that does not need a
`VmafContext` — e.g. a learned de-banding preprocessor that mutates a luma
plane before downstream processing.

```c
typedef struct VmafDnnSession VmafDnnSession;

int  vmaf_dnn_session_open (VmafDnnSession **out, const char *onnx_path, const VmafDnnConfig *cfg);
void vmaf_dnn_session_close(VmafDnnSession *sess);
```

`vmaf_dnn_session_open()` returns `0`, `-ENOSYS` (no DNN support), `-EINVAL`
(NULL `out` or `onnx_path`), `-ENOENT` (file missing), `-E2BIG` (over the
50 MB cap) or `-EIO` (ONNX Runtime failure). `cfg` may be `NULL` for
`VMAF_DNN_DEVICE_AUTO`. `vmaf_dnn_session_close()` accepts `NULL`, tears down
the ORT session and its binding caches, and invalidates any pointer from
`vmaf_dnn_session_attached_ep()`. Pair every successful open with one close.

Both `vmaf_dnn_session_open` and `vmaf_use_tiny_model` apply the same
size-cap + operator-allowlist walk. See
[ADR-0039](../adr/0039-onnx-runtime-op-walk-registry.md) for the allowlist
and [ADR-0041](../adr/0041-lpips-sq-extractor.md) for an example of an
extractor that uses a session under the hood.

### Luma-only convenience call

```c
int vmaf_dnn_session_run_luma8(VmafDnnSession *sess,
                               const uint8_t *in,  size_t in_stride,
                               int w, int h,
                               uint8_t *out,       size_t out_stride);
```

Runs one luma-in / luma-out pass. Only works when the ONNX graph has:

- exactly one float32 input of static shape `[1, 1, H, W]`,
- exactly one output of the same shape.

The implementation:

1. Reads `in` (uint8 luma), normalises to `[0, 1]` (applies mean/std from the
   sidecar JSON if present).
2. Runs ORT.
3. De-normalises, rounds, clamps to `[0, 255]`, writes `out`.

Errors:

- `-EINVAL` — `sess`, `in`, or `out` is NULL.
- `-ENOTSUP` — graph shape isn't the supported NCHW `[1,1,H,W]` luma layout,
  or ORT returned fewer output elements than `w*h`.
- `-ERANGE` — `w`/`h` don't match the graph's static input shape. Use
  `vmaf_dnn_session_run()` for dynamic shapes.
- `-ENOSYS` — libvmaf was built without DNN support (`enable_dnn`
  disabled); every DNN entry point returns `-ENOSYS` in that configuration.

### 10/12/16-bit convenience call

```c
int vmaf_dnn_session_run_plane16(VmafDnnSession *sess,
                                 const uint16_t *in,  size_t in_stride,
                                 int w, int h, int bpc,
                                 uint16_t *out,        size_t out_stride);
```

The bit-depth-extended sibling of `_luma8`. Used by the ffmpeg
`vmaf_pre` filter for `yuv420p10le` / `yuv422p10le` / `yuv444p10le`
(and the 12-bit LE counterparts), and — at any supported bit depth —
to run the same session on chroma planes at their sub-sampled
dimensions. Added in
[ADR-0170](../adr/0170-vmaf-pre-10bit-chroma.md) (T6-4).

- `in` / `out` are packed `uint16` little-endian single-plane
  buffers.
- `in_stride` / `out_stride` are in **bytes** (not samples) — same
  convention as `_luma8`, so a 10-bit 1920×1080 plane has
  `stride ≥ 1920 * 2`.
- `bpc` in range 9..16 selects the normalisation divisor
  `(1 << bpc) - 1`. Passing `bpc=8` returns `-EINVAL` — use
  `_luma8` for 8-bit input.

The model must still declare `[1, 1, H, W]` static shape; the only
new freedom is the bit depth of the host-side buffer the loader
normalises from. A single `learned_filter_v1` session works for
both luma and chroma — re-call with chroma W/H (the shape is
declared dynamic, see the open() comment).

Errors match `_luma8`, plus `-EINVAL` for a `bpc` outside `[9, 16]`.

### General named-binding call

For models with multiple inputs / outputs or non-luma shapes, use the
general call:

```c
typedef struct VmafDnnInput {
    const char    *name;   /* bind by graph name; NULL = positional */
    const float   *data;   /* row-major float32 */
    const int64_t *shape;  /* rank dims */
    size_t         rank;
} VmafDnnInput;

typedef struct VmafDnnOutput {
    const char *name;      /* bind by graph name; NULL = positional */
    float      *data;      /* caller-owned */
    size_t      capacity;  /* element count allocated */
    size_t      written;   /* OUT: element count produced */
} VmafDnnOutput;

int vmaf_dnn_session_run(VmafDnnSession *sess,
                         const VmafDnnInput *inputs,   size_t n_inputs,
                         VmafDnnOutput     *outputs,   size_t n_outputs);
```

Name-binding (`name != NULL`) resolves by the ONNX graph's declared input /
output names. Positional binding (`name == NULL`) uses the tensor's array
index. Mix is allowed but discouraged — pick one style per session.

Errors:

- `-ENOSYS` — built without DNN support.
- `-EINVAL` — mismatched arity, null pointers, rank zero.
- `-ENOMEM` — allocation failure (per-input staging buffer, or tensor
  creation).
- `-ENOSPC` — some `outputs[i].capacity` is smaller than the produced tensor.
  On this return, `outputs[i].written` is populated with the required
  element count (the code sets `written = produced` *before* the capacity
  check), so the caller can resize and retry with the same bindings.
- `-EIO` — ORT failure (bad graph, EP crash, OOM on device). The diagnostic
  is logged via the `VmafContext` log callback if one is configured (for
  sessions opened without a `VmafContext`, logging goes through the
  library's global log sink).

See [ADR-0040](../adr/0040-dnn-session-multi-input-api.md) for the rationale
behind multi-input/output + named binding.

### Which execution provider bound: `vmaf_dnn_session_attached_ep`

```c
const char *vmaf_dnn_session_attached_ep(VmafDnnSession *sess);
```

Returns the ONNX Runtime EP that actually bound to the session, for
diagnostics and for asserting AUTO-chain behaviour. The strings are `"CPU"`,
`"CUDA"`, `"ROCm"`, `"CoreML"`, `"CoreML:ANE"`, `"CoreML:GPU"`,
`"CoreML:CPU"`, `"OpenVINO:CPU"`, `"OpenVINO:GPU"` and `"OpenVINO:NPU"`.
It returns `NULL` when `sess` is `NULL` or libvmaf has no DNN support. The
string is owned by the session and invalid after
`vmaf_dnn_session_close()`. It is safe from any thread once the session is
open and no inference is running on it.

## Thread-safety

- A single `VmafDnnSession` is **not** re-entrant. Driving inference from two
  threads requires either per-thread sessions or external locking.
- Opening multiple sessions concurrently is safe; they do not share state
  beyond process-global ORT singletons.
- Attaching a tiny model via `vmaf_use_tiny_model()` is subject to the same
  single-driver rule as the rest of the `VmafContext` API — see
  [the overview](index.md#thread-safety).
- `vmaf_dnn_available()` and `vmaf_dnn_verify_signature()` are safe from any
  thread.

## Runnable example — standalone luma filter

```c
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <libvmaf/dnn.h>

int main(int argc, char **argv)
{
    if (!vmaf_dnn_available()) {
        fprintf(stderr, "libvmaf was built without DNN support\n");
        return 2;
    }

    VmafDnnSession *sess = NULL;
    VmafDnnConfig cfg = { .device = VMAF_DNN_DEVICE_AUTO };

    int err = vmaf_dnn_session_open(&sess, argv[1], &cfg);
    if (err < 0) {
        fprintf(stderr, "open failed: %d (%s)\n", err, strerror(-err));
        return 1;
    }

    const int W = 1920, H = 1080;
    uint8_t *in  = malloc((size_t)W * H);
    uint8_t *out = malloc((size_t)W * H);
    /* ...fill `in` with luma from your pipeline... */

    err = vmaf_dnn_session_run_luma8(sess, in, W, W, H, out, W);
    if (err < 0) fprintf(stderr, "run failed: %d\n", err);

    free(in); free(out);
    vmaf_dnn_session_close(sess);
    return err < 0 ? 1 : 0;
}
```

Build:

```bash
cc filter.c -o filter $(pkg-config --cflags --libs libvmaf)
```

Needs a libvmaf built with DNN support (`-Denable_dnn=enabled`).

## Sigstore signature verification: `vmaf_dnn_verify_signature`

Verify a tiny model's Sigstore bundle before loading it. The same check is
behind the `vmaf` CLI flag `--tiny-model-verify`
([CLI usage](../usage/cli.md)).

```c
int vmaf_dnn_verify_signature(const char *onnx_path, const char *registry_path);
```

| Argument | Meaning |
| --- | --- |
| `onnx_path` | Path to the model file. Its basename is the registry key. |
| `registry_path` | Explicit registry file, or `NULL` to use `registry.json` next to the model. |

The function:

1. Reads the registry and finds the entry for the model's basename.
2. Reads that entry's `sigstore_bundle` field and resolves the bundle path.
3. Runs `cosign verify-blob` through `posix_spawnp`, pinned to the identity
   regexp `https://github.com/VMAFx/vmafx/.github/workflows/.+` and the
   issuer `https://token.actions.githubusercontent.com`.
4. Returns `0` only when cosign exits `0`. It fails closed: any error
   stops model load.

| Return | Meaning |
| --- | --- |
| `0` | Verification passed. |
| `-ENOENT` | Registry missing, bundle missing, or no entry for the model. |
| `-EACCES` | `cosign` is not on `PATH`. |
| `-EPROTO` | `cosign` exited non-zero (bad or non-matching signature). |
| `-ENOSYS` | Windows build: the supply-chain path needs POSIX process spawning. |
| `-EINVAL` | `onnx_path` is `NULL`. This check runs before the platform check. |

There is no error-text out parameter; the CLI prints the negated errno. Any
cosign diagnostics go to the process's own stderr.

!!! note "CLI coupling"
    With `--tiny-model-verify` the CLI calls this function itself, before
    `vmaf_use_tiny_model()`, and refuses to load the model on a nonzero
    result (`--tiny-model-verify: signature verification failed for <path>
    (errno N)`). `vmaf_use_tiny_model()` does not verify by itself.

Provenance: the bundle comes from the fork's release-please and Sigstore
signing pipeline ([ADR-0010](../adr/0010-sigstore-keyless-signing.md), and
the policy in [the model registry](../ai/model-registry.md)). Bundles under
`model/tiny/` carry a keyless OIDC identity tied to the GitHub Actions
workflow that built the model. The function is part of the model-registry
work in [ADR-0211](../adr/0211-model-registry-sigstore.md).

## Testing standalone sessions

Run the session and ORT-internals tests with:

```sh
python3 "$(git rev-parse --show-toplevel)/scripts/ci/run_meson_test.py" -- \
  -C build --print-errorlogs test_ort_internals test_dnn_session_api
```

- Use a build configured with `-Denable_dnn=enabled` and a discoverable ONNX
  Runtime package to exercise inference and session creation. A
  disabled-DNN build exercises only the stub semantics, so that result alone
  does not validate inference.
- The POSIX fixture helper checks that source read errors and destination
  close or flush failures cannot be reported as successful copies. The
  close-error case runs first, before ORT initialisation, and confines its
  file-size limit and signal disposition to a child process. The read-error
  case stays last.

## Known limitations

- **Attached mode supports multiple scalar output tensors, not vector or image
  output tensors.** `vmaf_use_tiny_model()` records every ONNX output
  when each output tensor contains exactly one scalar value. If any attached
  output tensor has more than one element, the frame run
  returns `-ENOTSUP`. Use the standalone `vmaf_dnn_session_run()` API for
  caller-owned vector/image output buffers. See
  [ADR-0646](../adr/0646-dnn-attached-multi-output.md).
- **Attached mode caps routed outputs at eight tensors.** This mirrors
  `VMAF_ORT_MAX_IO`, the existing ORT wrapper stack-array limit. Models with
  more outputs should collapse related values into a standalone session output
  or ship a future ADR that raises the cap across the ORT wrapper and sidecar
  parser together.

- Operator allowlist covers the set required by tiny FR / NR / filter models
  shipped in `model/tiny/`; untrusted models with new op types will be
  rejected at `_open`. Extend the allowlist via the registry — see
  [ADR-0039](../adr/0039-onnx-runtime-op-walk-registry.md).
- EP selection is a preference, not a requirement (see
  [which EP bound](#which-execution-provider-bound-vmaf_dnn_session_attached_ep)).
- There is no callback / progress hook; inference is synchronous per call.
- Sessions are heap-only; no stack-allocated variant.

## Related

- [ADR-0022](../adr/0022-inference-runtime-onnx.md) — choice of ONNX Runtime.
- [ADR-0023](../adr/0023-tinyai-user-surfaces.md) — where the CLI / C API /
  ffmpeg / training surfaces intersect.
- [ADR-0036](../adr/0036-tinyai-wave1-scope-expansion.md) — Wave 1 scope
  (LPIPS, saliency, per-shot CRF, `vmaf_post`, allowlist `Loop`/`If`, MCP VLM).
- [ADR-0039](../adr/0039-onnx-runtime-op-walk-registry.md) — operator
  allowlist + model registry.
- [ADR-0040](../adr/0040-dnn-session-multi-input-api.md) — multi-input/output
  named-binding API.
- [ADR-0041](../adr/0041-lpips-sq-extractor.md) — LPIPS-SqueezeNet extractor
  (consumer of this API).
- [ADR-0042](../adr/0042-tinyai-docs-required-per-pr.md) — tiny-AI doc
  specialisation.
- [../ai/inference.md](../ai/inference.md) — CLI-side tiny-AI walkthrough.
