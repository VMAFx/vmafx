<!-- markdownlint-disable MD013 MD024 MD026 MD060 -->
# MCP tool reference

Per-tool request / response schemas and error semantics for the MCP
servers ([overview](index.md)). The Go server `vmafx-mcp` serves all 24
tools; the Python server `vmaf-mcp` serves the first 19 in the table below.
Source of truth: the tool registrations in
[cmd/vmafx-mcp/tools.go](../../cmd/vmafx-mcp/tools.go) and the `list_tools()`
handler in
[mcp-server/vmaf-mcp/src/vmaf_mcp/server.py](../../mcp-server/vmaf-mcp/src/vmaf_mcp/server.py).
The names, property types and required arguments the two servers share are
recorded in
[mcp-server/vmaf-mcp/tool-contract.json](../../mcp-server/vmaf-mcp/tool-contract.json),
written from the Python handler and checked against both servers
([Tests](index.md#tests)).

Every tool returns a single `TextContent` message whose body is a JSON
document. On error the body has shape `{"error": "<string>"}`, so clients
can `json.loads()` unconditionally and branch on the presence of
`error`.

## Tool summary

| Tool | Arguments | Servers | Execution |
| --- | --- | --- | --- |
| [`vmaf_score`](#vmaf_score) | `ref`, `dis`, `width`, `height`, `pixfmt`, `bitdepth`, optional `model`, `backend`, `subsample`, `precision`, scoring extras | Python, Go | CLI subprocess; Go: optional cgo |
| [`list_models`](#list_models) | none | Python, Go | filesystem |
| [`list_backends`](#list_backends) | none | Python, Go | `vmaf --list-backends` probe |
| [`run_benchmark`](#run_benchmark) | none | Python, Go | `bench_all.sh` |
| [`eval_model_on_split`](#eval_model_on_split) | `model`, `features`, `split`, `input_name` | Python, Go | native |
| [`compare_models`](#compare_models) | `models`, `features`, `split` | Python, Go | native |
| [`describe_worst_frames`](#describe_worst_frames) | `ref`, `dis`, `width`, `height`, `pixfmt`, `bitdepth`, optional `n` | Python, Go | CLI + VLM |
| [`probe_backend`](#probe_backend) | `backend` | Python, Go | CLI probe |
| [`vmaf_version`](#vmaf_version) | none | Python, Go | `vmaf --version` |
| [`vmaf_score_encoded`](#vmaf_score_encoded) | `reference_encoded`, `distorted_encoded`, optional `model`, `backend`, `subsample`, `precision` | Python, Go | `ffmpeg` + CLI |
| [`list_extractors`](#list_extractors) | none | Python, Go | `vmaf` probe |
| [`describe_model`](#describe_model) | `name` | Python, Go | CLI; Go: optional cgo |
| [`run_compare`](#run_compare), [`run_ladder`](#run_ladder), [`run_tune_per_shot`](#run_tune_per_shot) | see each tool | Python, Go | `vmaf-tune` |
| [`vmaf_per_shot`](#vmaf_per_shot), [`vmaf_roi`](#vmaf_roi), [`vmaf_bench`](#vmaf_bench), [`vmaf_vpl`](#vmaf_vpl) | see each tool | Python, Go | sidecar binaries |
| [`submit_job`](#submit_job), [`get_job`](#get_job), [`cancel_job`](#cancel_job), [`list_jobs`](#list_jobs), [`vmaf_score_remote`](#vmaf_score_remote) | see each tool | Go only | gRPC |

## `vmaf_score`

Score one `(ref, dis)` YUV pair and return the full VMAF JSON report.

### Input schema

The scoring tools (`vmaf_score`, `vmaf_score_encoded`, `describe_worst_frames`)
take the arguments below; the **Tools** column names the tools that accept
each one. Both servers serve these schemas from one generated file
(`options.gen.json`, generated from the option groups of
`core/api/vmafx.toml`), so the Go and Python servers cannot drift, and each
argument reaches the same `vmaf` flag (CLI column of
[the CLI reference](../usage/cli.md#option-reference)). A `model` that is not
given is the library default model (`VMAF_DEFAULT_MODEL_VERSION`,
`vmaf_v1.0.16_3d0h` in this release); pass `model="version=vmaf_v0.6.1"` to
reproduce the numbers of releases before 1.0.0-rc.4.

<!-- BEGIN GENERATED: vmafx-api mcp scoring arguments (scripts/codegen/vmafx-api.py) -->

| Argument | Type | Required | Default | Tools | Description |
| --- | --- | --- | --- | --- | --- |
| `ref` | string | yes | | `vmaf_score`, `describe_worst_frames` | Reference video: a .y4m file, or raw planar .yuv together with the width, height, pixel format and bit depth. |
| `dis` | string | yes | | `vmaf_score`, `describe_worst_frames` | Distorted video, in the same form as the reference. |
| `width` | uint >= 1 | yes | | `vmaf_score`, `describe_worst_frames` | Width of raw .yuv input in pixels. |
| `height` | uint >= 1 | yes | | `vmaf_score`, `describe_worst_frames` | Height of raw .yuv input in pixels. |
| `pixfmt` | `420` \| `422` \| `444` | yes | | `vmaf_score`, `describe_worst_frames` | Chroma subsampling of raw .yuv input. |
| `bitdepth` | `8` \| `10` \| `12` \| `16` | yes | | `vmaf_score`, `describe_worst_frames` | Bits per sample of raw .yuv input. |
| `reference_encoded` | string | yes | | `vmaf_score_encoded` | Path to the reference encoded video (MP4/MKV/Y4M/...). Must be under an allowlisted root (VMAF_MCP_ALLOW). |
| `distorted_encoded` | string | yes | | `vmaf_score_encoded` | Path to the distorted encoded video. |
| `model` | string | | library default (`VMAF_DEFAULT_MODEL_VERSION`) | `vmaf_score`, `vmaf_score_encoded`, `describe_worst_frames` | Model, colon-delimited: version= a built-in model, path= a model file, name= the name in the report, disable_clip, enable_transform, &lt;feature&gt;.&lt;option&gt;=&lt;value&gt; overloads. Several models score in one pass (repeat the option; the filter separates them with \|). |
| `disable_clip` | bool | | `false` | `vmaf_score`, `vmaf_score_encoded` | Do not clip the model score to [0, 100] (the model's disable_clip). |
| `enable_transform` | bool | | `false` | `vmaf_score`, `vmaf_score_encoded` | Apply the model's score transform (the model's enable_transform). |
| `backend` | `auto` \| `cpu` \| `cuda` \| `sycl` \| `hip` \| `metal` | | `auto` | `vmaf_score`, `vmaf_score_encoded`, `describe_worst_frames` | Backend: auto uses the available ones; any other value runs that backend alone and fails when it is not available. |
| `feature` | string (list) | | | `vmaf_score`, `vmaf_score_encoded` | Additional feature extractor, name[=key=value:...] (for example psnr or cambi=full_ref=true); several may be given (the filter separates them with \|). Mutually exclusive with the CTC presets. |
| `aom_ctc` | `v1.0` \| `v2.0` \| `v3.0` \| `v4.0` \| `v5.0` \| `v6.0` \| `v7.0` | | | `vmaf_score`, `vmaf_score_encoded` | AOM common test conditions preset: a fixed model and feature set. |
| `nflx_ctc` | `v1.0` | | | `vmaf_score`, `vmaf_score_encoded` | Netflix common test conditions preset: a fixed model and feature set. |
| `tiny_model` | string | | | `vmaf_score`, `vmaf_score_encoded` | Tiny ONNX model to load alongside the classic models. |
| `tiny_device` | `auto` \| `cpu` \| `cuda` \| `openvino` \| `openvino-npu` \| `openvino-cpu` \| `openvino-gpu` \| `coreml` \| `coreml-ane` \| `coreml-gpu` \| `coreml-cpu` \| `rocm` | | `auto` | `vmaf_score`, `vmaf_score_encoded` | ONNX Runtime execution provider of the tiny model. |
| `dnn_ep` | `auto` \| `cpu` \| `cuda` \| `openvino` \| `openvino-npu` \| `openvino-cpu` \| `openvino-gpu` \| `coreml` \| `coreml-ane` \| `coreml-gpu` \| `coreml-cpu` \| `rocm` | | | `vmaf_score`, `vmaf_score_encoded` | Alias of the tiny-model device under the ONNX Runtime name (execution provider); both set the same setting. |
| `tiny_threads` | uint | | | `vmaf_score`, `vmaf_score_encoded` | Intra-op threads of the CPU execution provider (0: the runtime's default). |
| `tiny_fp16` | bool | | `false` | `vmaf_score`, `vmaf_score_encoded` | Request fp16 input and output where the execution provider supports it. |
| `tiny_model_verify` | bool | | `false` | `vmaf_score`, `vmaf_score_encoded` | Require a Sigstore bundle verification of the tiny model (cosign verify-blob) before it loads; a missing bundle, a missing cosign or a failed verification refuses the model. |
| `tiny_codec` | string | | | `vmaf_score`, `vmaf_score_encoded` | Encoder of the distorted clip, required by codec-aware tiny models (fr_regressor_v2/v3), which refuse to score without it. Must be in the model sidecar's encoder_vocab; the ffprobe names h264, hevc, av1, vp9 and vvc are accepted. |
| `tiny_preset` | string | | | `vmaf_score`, `vmaf_score_encoded` | Encoder preset (medium, slow, p4, 5, ...), read as the encoder defines it. Unset: ordinal 5 (medium). A model trained with one preset (fr_regressor_v3) ignores it and warns. |
| `tiny_crf` | uint 0..63 | | | `vmaf_score`, `vmaf_score_encoded` | CRF or QP used for the encode, normalised as the model sidecar declares. Required with the codec and preset. |
| `tiny_resize` | `bilinear` \| `nearest` \| `bicubic` \| `disabled` | | `disabled` | `vmaf_score`, `vmaf_score_encoded` | Resize filter for NCHW tiny models whose input size differs from the frame; disabled refuses the mismatch (-ERANGE). The three filters give scores about 2% apart: record the filter with the model. |
| `no_reference` | bool | | `false` | `vmaf_score`, `vmaf_score_encoded` | No-reference mode; needs a no-reference tiny model. The reference becomes a formality: only the distorted picture is scored. |
| `threads` | uint | | | `vmaf_score`, `vmaf_score_encoded` | Worker threads of the feature extractors, capped to the hardware threads (0: score in the calling thread). |
| `frame_cnt` | uint >= 1 | | | `vmaf_score`, `vmaf_score_encoded` | Score at most this many frames. |
| `frame_skip_ref` | uint | | | `vmaf_score`, `vmaf_score_encoded` | Skip this many frames at the start of the reference. |
| `frame_skip_dist` | uint | | | `vmaf_score`, `vmaf_score_encoded` | Skip this many frames at the start of the distorted video. |
| `no_prediction` | bool | | `false` | `vmaf_score`, `vmaf_score_encoded` | Extract features only; no model score. |
| `cpumask` | uint | | | `vmaf_score`, `vmaf_score_encoded` | Bitmask of CPU instruction sets the extractors must not use. |
| `gpumask` | uint | | | `vmaf_score`, `vmaf_score_encoded` | Bitmask of GPU operations the extractors must not use. |
| `sycl_device` | uint | | | `vmaf_score`, `vmaf_score_encoded` | SYCL GPU by index (unset: selected automatically). |
| `hip_device` | uint | | | `vmaf_score`, `vmaf_score_encoded` | HIP GPU by index (opt-in: HIP is off unless this or --backend hip is given). |
| `metal_device` | uint | | | `vmaf_score`, `vmaf_score_encoded` | Metal GPU by index (opt-in: Metal is off unless this or --backend metal is given). |
| `subsample` | uint >= 1 | | `1` | `vmaf_score`, `vmaf_score_encoded` | Score every n-th frame (1: every frame). |
| `precision` | string | | `legacy` | `vmaf_score`, `vmaf_score_encoded` | Score precision: N (1..17) writes %.&lt;N&gt;g; max or full write %.17g (round-trip lossless); legacy writes %.6f (Netflix-compatible). The scoring server returns lossless scores unless asked otherwise. |
| `output_fmt` | `json` \| `xml` \| `csv` \| `sub` | | `json` | `vmaf_score`, `vmaf_score_encoded` | Report format; json and xml carry the backend receipt. |
| `csv` | bool | | `false` | `vmaf_score`, `vmaf_score_encoded` | Write the report as CSV; the same as output_fmt csv. |
| `sub` | bool | | `false` | `vmaf_score`, `vmaf_score_encoded` | Write per-frame scores as subtitles; the same as output_fmt sub. |
| `view_distance` | float 0.75..24 | | | `vmaf_score`, `vmaf_score_encoded` | Viewing distance in display heights (ADM adm_norm_view_dist). Unset: the model's value. |
| `display_height` | uint >= 1 | | | `vmaf_score`, `vmaf_score_encoded` | Height of the reference display in pixels (ADM adm_ref_display_height). Unset: the model's value. |
| `target_width` | uint | | `0` | `vmaf_score`, `vmaf_score_encoded` | Width of the target display the distorted video is scaled to (0: no scaling). Reserved: device-targeted scoring lands in RC5; only the default is accepted. |
| `target_height` | uint | | `0` | `vmaf_score`, `vmaf_score_encoded` | Height of the target display the distorted video is scaled to (0: no scaling). Reserved: device-targeted scoring lands in RC5; only the default is accepted. |
| `target_scaling` | `none` \| `bilinear` \| `bicubic` \| `lanczos` | | `none` | `vmaf_score`, `vmaf_score_encoded` | Scaling filter towards the target display. Reserved: device-targeted scoring lands in RC5; only the default is accepted. |
| `n` | uint 1..32 | | `5` | `describe_worst_frames` | How many worst-VMAF frames to describe. |

<!-- END GENERATED: vmafx-api mcp scoring arguments -->

All [optional scoring parameters](#optional-scoring-parameters) below
(`feature`, `aom_ctc`/`nflx_ctc`, the `tiny_*` / `no_reference` tiny-AI
surface, and the frame-range controls) are also accepted on `vmaf_score`.

### Behaviour

The server exec's the local `vmaf` binary with, effectively:

```bash
vmaf -r <ref> -d <dis> --width <w> --height <h> -p <pixfmt> -b <bitdepth> \
     -m <model> --precision <precision> -q --json -o <tmp>
# plus per-backend flags disabling sibling backends:
#   backend=cpu   → --no_cuda --no_sycl --no_hip --no_metal
#   backend=cuda  → --no_sycl --no_hip --no_metal
#   backend=sycl  → --no_cuda --no_hip --no_metal
#   backend=hip   → --no_cuda --no_sycl --no_metal
#   backend=metal → --no_cuda --no_sycl --no_hip
```

The JSON written by vmaf is parsed and returned with two backend receipt fields
guaranteed by the MCP response (ADR-0495):

- `backend_requested` — verbatim echo of the caller's `backend` arg.
- `backend_used` — what actually ran. For an explicit `backend` arg
  this equals the requested value (both wrappers refuse to silently
  fall back). For `backend="auto"`, the Python and Go wrappers preserve the
  `backend_used` receipt emitted by the fork's `vmaf` CLI (`"cpu"`,
  `"cuda"`, `"sycl"`, `"hip"`, or `"metal"`). Older or external
  binaries that omit a valid receipt produce `"unknown"`; neither wrapper
  guesses from the number of metric keys because that count changes
  as extractors evolve.

When the local `vmaf` binary does not advertise the requested
backend, the wrapper raises rather than running CPU silently
(the bug-1 pattern from the 2026-05-17 probe). Use
`backend="auto"` to opt back into vmaf's own probe.

The wrapper additionally emits a `mismatched_model_warning` field
when the model's intended resolution preset disagrees with the
source frame size — e.g. `version=vmaf_4k_v0.6.1` on a 576×324
source saturates at 100 on every frame and the warning surfaces
the foot-gun. Bespoke ONNX models with no known resolution
preset are silent (no false positives). See
[usage/cli.md](../usage/cli.md#output) for the rest of the report
schema; the temp file is always unlinked, even on error.

> **`precision` default `"legacy"`.** The MCP server passes
> `--precision legacy` (`%.6f`, Netflix-compatible) by default, matching
> the underlying `vmaf` CLI default per
> [ADR-0119](../adr/0119-cli-precision-default-revert.md). Pass `"max"`
> (or `"17"`, i.e. `%.17g`, IEEE-754 round-trip lossless) when a
> programmatic consumer needs scores that re-parse to the exact same
> double.

### Example call

```json
{
  "method": "tools/call",
  "params": {
    "name": "vmaf_score",
    "arguments": {
      "ref":      "python/test/resource/yuv/src01_hrc00_576x324.yuv",
      "dis":      "python/test/resource/yuv/src01_hrc01_576x324.yuv",
      "width":    576,
      "height":   324,
      "pixfmt":   "420",
      "bitdepth": 8,
      "backend":  "cpu"
    }
  }
}
```

Response body (abridged):

```json
{
  "version": "1.0.0-rc.2",
  "pooled_metrics": { "vmaf": { "mean": 76.667831, "...": "..." } },
  "frames": [ { "frameNum": 0, "metrics": { "vmaf": 78.8263, "...": "..." } } ]
}
```

### Errors

- Path not under an allowlisted root →
  `{"error": "path ... not under an allowlisted root; set VMAF_MCP_ALLOW to extend."}`.
- Path does not exist → `{"error": "<abs-path>"}` from `FileNotFoundError`.
- vmaf binary missing →
  `{"error": "vmaf binary not found at ...; Build first: meson compile -C build."}`.
- Non-zero vmaf exit → `{"error": "vmaf exited <code>: <stderr>"}`.
- Caller-requested backend not advertised by the local binary →
  `{"error": "backend 'cuda' requested but the local vmaf binary
  does not advertise it (available: ['cpu']); refusing to fall back
  silently. Pass backend='auto' to let vmaf pick, or rebuild with
  the requested backend enabled."}` (ADR-0495).

### Optional scoring parameters

These optional parameters (ADR-1117) are accepted on **both**
`vmaf_score` and `vmaf_score_encoded`. Each maps onto a `vmaf` CLI flag
verified against `core/tools/cli_parse.c`, and is forwarded **only when
supplied** — omitting them leaves the score identical to a call without
them, so existing callers are unaffected. The Go (`cmd/vmafx-mcp`) and
Python (`mcp-server/vmaf-mcp`) servers expose a byte-identical schema for
all of them.

#### Tiny-AI / DNN scoring

This is the fork's ONNX tiny-model surface (previously unreachable over
MCP).

| Field               | Type / values                                                                                                              | CLI flag              | Notes                                                                                              |
| --- | --- | --- | --- |
| `tiny_model`        | string (path)                                                                                                              | `--tiny-model`        | Load a tiny ONNX model alongside the classic models.                                               |
| `tiny_device`       | `auto \| cpu \| cuda \| openvino \| openvino-npu \| openvino-cpu \| openvino-gpu \| coreml \| coreml-ane \| coreml-gpu \| coreml-cpu \| rocm` | `--tiny-device` (= `--dnn-ep`) | ONNX Runtime execution provider. Default `auto`.                                       |
| `dnn_ep`            | `auto \| cpu \| cuda \| openvino \| openvino-npu \| openvino-cpu \| openvino-gpu \| coreml \| coreml-ane \| coreml-gpu \| coreml-cpu \| rocm` | `--dnn-ep` (= `--tiny-device`) | Alias for `tiny_device` matching the `--dnn-ep` CLI flag.                                          |
| `tiny_threads`      | integer `≥ 0`                                                                                                               | `--tiny-threads`      | CPU EP intra-op threads (`0` = ORT default).                                                        |
| `tiny_fp16`         | boolean                                                                                                                     | `--tiny-fp16`         | Request fp16 IO where the EP supports it.                                                           |
| `tiny_model_verify` | boolean                                                                                                                     | `--tiny-model-verify` | Require Sigstore-bundle verification before loading the model.                                      |
| `tiny_codec`        | string                                                                                                                      | `--tiny-codec`        | Encoder name for codec-aware tiny models (e.g. `libx264`).                                          |
| `tiny_preset`       | string                                                                                                                      | `--tiny-preset`       | Encoder preset string for codec-aware tiny models.                                                  |
| `tiny_crf`          | integer `0..63`                                                                                                            | `--tiny-crf`          | CRF / QP for codec-aware tiny models (clamped to `0..63`).                                          |
| `tiny_resize`       | `bilinear \| nearest \| bicubic \| disabled`                                                                                | `--tiny-resize`       | Auto-resize filter for NCHW tiny models on a dimension mismatch. Default `disabled` (hard-errors).  |
| `no_reference`      | boolean                                                                                                                     | `--no-reference`      | No-reference (NR) mode — see below.                                                                 |

**Input validation.** MCP inputs are untrusted and validated strictly before
spawning the CLI:

- Enums (`tiny_device`, `dnn_ep`, `tiny_resize`, `aom_ctc`, `nflx_ctc`,
  `pixfmt`, `backend`) reject unknown values.
- Conflicting `tiny_device` and `dnn_ep` values are rejected.
- Numeric bounds are enforced (`tiny_crf` in `0..63`, `tiny_threads ≥ 0`,
  `threads ≥ 1`, `frame_cnt ≥ 1`, `frame_skip_ref ≥ 0`, `frame_skip_dist ≥ 0`,
  `subsample ≥ 1`, `bitdepth` in `{8, 10, 12, 16}`).

**No-reference mode.** When `no_reference` is set, only the distorted
picture is scored, so `no_reference` **requires** `tiny_model` (an NR
ONNX model — there is no classic NR scorer). The request is rejected with
`{"error": "no_reference requires tiny_model; no classic NR scorer
exists"}` otherwise, mirroring the CLI's `cli_parse.c` gate. In NR mode
the `ref` argument becomes optional on `vmaf_score`: omit it, or pass any
valid YUV of matching geometry (it is not consumed by the scorer). For
`vmaf_score_encoded` the reference video is still decoded as usual.

#### Feature selection and CTC presets

| Field      | Type / values                                            | CLI flag      | Notes                                                                                          |
| --- | --- | --- | --- |
| `feature`  | array of strings                                         | `--feature`   | Each entry becomes a repeated `--feature` flag. Use the libvmaf `name[=key=val:...]` grammar.   |
| `aom_ctc`  | `v1.0 \| v2.0 \| v3.0 \| v4.0 \| v5.0 \| v6.0 \| v7.0`    | `--aom_ctc`   | AOM Common Test Conditions preset.                                                             |
| `nflx_ctc` | `v1.0`                                                    | `--nflx_ctc`  | Netflix Common Test Conditions preset.                                                         |

The `aom_ctc` / `nflx_ctc` presets configure a fixed model + feature set
and are **mutually exclusive with manual feature/model configuration** —
combining them with `feature` or a custom `model` stacks both
configurations (the CLI does not reject it, but the result is rarely what
you want).

#### Frame-range and worker controls

| Field             | Type          | CLI flag            | Notes                                                  |
| --- | --- | --- | --- |
| `threads`         | integer `≥ 1` | `--threads`         | Worker threads (capped to hardware cores by the CLI).  |
| `frame_cnt`       | integer `≥ 1` | `--frame_cnt`       | Maximum number of frames to process.                   |
| `frame_skip_ref`  | integer `≥ 0` | `--frame_skip_ref`  | Skip the first N reference frames.                     |
| `frame_skip_dist` | integer `≥ 0` | `--frame_skip_dist` | Skip the first N distorted frames.                     |
| `no_prediction`   | boolean       | `--no_prediction`   | Extract features only; skip VMAF prediction.           |

#### Device selectors

Control hardware device selection and hardware capability masks on
heterogeneous systems (#1240).

| Field          | Type          | CLI flag         | Notes                                                        |
| --- | --- | --- | --- |
| `cpumask`      | integer `≥ 0` | `--cpumask`      | Bitmask restricting permitted CPU SIMD instruction sets.     |
| `gpumask`      | integer `≥ 0` | `--gpumask`      | Bitmask restricting permitted GPU operations.                |
| `sycl_device`  | integer `≥ 0` | `--sycl_device`  | Select SYCL GPU device by index.                             |
| `hip_device`   | integer `≥ 0` | `--hip_device`   | Select HIP GPU device by index.                              |
| `metal_device` | integer `≥ 0` | `--metal_device` | Select Metal GPU device by index.                            |

#### Output format

Select the serialization format emitted by the underlying `vmaf` engine (#1240).

| Field        | Type / values                       | CLI flag                           | Notes                                                              |
| --- | --- | --- | --- |
| `output_fmt` | `json \| xml \| csv \| sub`         | `--json \| --xml \| --csv \| --sub`| Output format. Default `json`. Non-JSON formats return a structured text payload (`{"format": ..., "output": ...}`). |

## `list_models`

Walk `model/` (recursively) and list every `.json`, `.pkl`, or `.onnx`
file shipped with the build.

### Input schema — no arguments.

### Response body

```json
{
  "models": [
    {
      "name": "vmaf_v0.6.1",
      "path": "model/vmaf_v0.6.1.json",
      "format": "json",
      "size_bytes": 9128
    },
    {
      "name": "lpips_sq_small",
      "path": "model/tiny/lpips_sq_small.onnx",
      "format": "onnx",
      "size_bytes": 4873216
    }
  ]
}
```

`name` is the file stem (no extension). Use it with `vmaf_score`'s
`model` field as `"version=<name>"` for built-in `.json` models or as a
plain path for custom `.pkl` / `.onnx`.

### Errors — none in the normal case (an empty `model/` returns `{"models": []}`).

## `list_backends`

Probe the local vmaf binary and report which runtime backends it was
built with.

### Input schema — no arguments.

### Response body

```json
{
  "cpu":    true,
  "cuda":   true,
  "sycl":   false,
  "hip":    false,
  "metal":  false
}
```

The server runs `vmaf --list-backends` and reports a backend `true` when it
was compiled in and its state initialises on this host; `cpu` is always
reported `true` when the binary exists. A backend that initialises can still
fail on a particular score (a driver fault mid-run). Use `probe_backend` to
verify that a backend can actually run a score.

### Errors

- If the vmaf binary is missing, `cpu` stays `true` and every GPU flag is
  `false` — no error is raised. Call `list_backends` before other tools to test
  whether the
  build is usable.

## run_benchmark

Run the full multi-fixture benchmark suite (`testdata/bench_all.sh`) against all
available compiled-in backends (CPU, CUDA, SYCL, HIP, Metal — Vulkan removed in
ADR-0726) on three canonical YUV fixture pairs built into the harness:

1. **576×324, 48 frames, 8-bit** — the Netflix golden pair
   `src01_hrc00 / src01_hrc01`
2. **1920×1080, 5 frames, 8-bit** — the 5-frame 1080p pair
3. **3840×2160, 200 frames, 8-bit** — the 4K BBB excerpt (`testdata/bbb/`)

For each fixture the harness scores all compiled-in backends, prints per-backend
VMAF means
and wall times, and prints a comparison table showing max per-frame diff between
CPU and each GPU backend. See [usage/bench.md](../usage/bench.md) for more
detail.

!!! warning "No per-call arguments"
    This tool does not accept per-call `ref`/`dis` arguments. Per-pair
    scoring is the job of `vmaf_score`; `bench_all.sh` is a fixed-fixture
    harness (ADR-0517).

!!! note "Protocol note"
    `run_benchmark` runs the full 4K test, which takes 30–60 seconds on a
    modern GPU. Real MCP clients hold the connection open. The heredoc test
    pattern (`docker exec -i ... vmaf-mcp << EOF ... EOF`) makes the server
    shut down on stdin EOF before the benchmark completes. Use a persistent
    pipe (`sleep 120 |`) when testing from the command line. ADR-0517
    preserves the diagnosis and repair rationale for the original failure.

!!! note "Error contract"
    `run_benchmark` raises `RuntimeError("benchmark failed — no output line
    containing pooled score / Pearson correlation")` on partial or silent
    pipe failures (ADR-0638). A second legacy implementation that swallowed
    the failure and returned a partial dict was removed in PR #517
    (Layer-5). MCP clients should branch on `isError=True`; see
    [Cross-tool error conventions](#cross-tool-error-conventions).

### Input schema

Takes no arguments.

```json
{}
```

### Response body

```json
{
  "exit_code": 0,
  "stdout": "=========================================\nTest 1: Official 576x324 (48 frames, 8-bit)\n...",
  "stderr": ""
}
```

The `stdout` field contains the full human-readable benchmark output.
Per-backend JSON
result files are written to `/tmp/vmaf-bench-<pid>/` (or to `VMAF_BENCH_OUTDIR`
if set).

### Errors

- `testdata/bench_all.sh` missing → raises `FileNotFoundError` with path.
- Non-zero `exit_code` is not itself an error field — both stdout and stderr are
  always returned so the caller can diagnose partial failures.
- Non-zero exit + empty stdout + empty stderr → `error` key is added with a
  root-cause shortlist and a `bash -x` re-run hint. Common causes: missing vmaf
  binary or missing fixture YUVs under `testdata/bbb/`.
- Unavailable backends (for example HIP scaffold-only) produce a `SKIP`
  line in stdout and do not abort the harness.

## `eval_model_on_split`

Load an ONNX tiny-AI regressor, run it against a parquet feature cache,
filter to a deterministic `train` / `val` / `test` split (keyed by the
`key` column via SHA-256 bucketing — same scheme as `vmaf_train`), and
report correlations against the `mos` target.

**Python server** (`vmaf-mcp`) requires the optional `eval` extra, which pulls
in `numpy`, `pandas`, `scipy`, and `onnxruntime`:

```bash
pip install -e 'mcp-server/vmaf-mcp[eval]'
```

**Go server** (`vmafx-mcp`) needs no Python at all. It reads the parquet
with a pure-Go reader, computes the split bucketing and the statistics in
`pkg/modeleval`, and runs the ONNX forward pass through libvmaf's own
ONNX Runtime session API. The one requirement is that libvmaf was built
with DNN support (`meson setup … -Denable_dnn=enabled`, or the `auto`
default on a host where ONNX Runtime is discoverable via pkg-config). A
libvmaf built without it returns:

```json
{"error": "libvmaf was built without DNN support (ONNX Runtime not found at build time); rebuild with -Denable_dnn=enabled against an installed onnxruntime"}
```

which is the Go counterpart of the Python server's missing-`[eval]`-extra error.

### Numerical parity between the two servers

The Python path hands scipy `float32` arrays and scipy does not upcast,
so its PLCC and RMSE carry float32 rounding. The Go path computes in
float64. Expect agreement to roughly `1e-7` on `plcc`, `1e-9` on `rmse`,
and exact agreement on `srocc` (rank-based, so width-independent) — not
bit-equality. The Go values are the more accurate of the two.

One deliberate behavioural difference: when a split is degenerate (every
prediction identical, so the correlation is mathematically undefined),
scipy returns `NaN` and Python emits a bare `NaN`, which is not valid
JSON. The Go server instead fails with an explicit
`correlation undefined: input has zero variance` error.

### Input schema

| Field        | Type                                                         | Required | Default      |
| --- | --- | --- | --- |
| `model`      | string (path to `.onnx`)                                     | yes      | —            |
| `features`   | string (path to `.parquet`)                                  | yes      | —            |
| `split`      | `"train" \| "val" \| "test" \| "all"`                        | no       | `"test"`     |
| `input_name` | string — the ONNX graph's input-tensor name                  | no       | `"features"` |

### Feature-column contract

The parquet must contain the column `mos` (ground-truth subjective
score). For the input tensor, the server picks whichever of these
columns are present:

- `adm2`
- `vif_scale0`, `vif_scale1`, `vif_scale2`, `vif_scale3`
- `motion2`

At least one must be present, in that order. The ONNX model must
accept a `float32` tensor of shape `[N, K]` where `K` is the number
of columns found.

### Response body

```json
{
  "model":    "/home/you/dev/vmaf/model/tiny/lpips_sq_small.onnx",
  "features": "/home/you/feature-cache/netflix-public.parquet",
  "split":    "test",
  "n":        137,
  "plcc":     0.9743,
  "srocc":    0.9612,
  "rmse":     3.214,
  "columns":  ["adm2", "vif_scale0", "vif_scale1", "vif_scale2", "vif_scale3", "motion2"]
}
```

### Errors

- Bad split name →
  `{"error": "split must be one of ('train', 'val', 'test', 'all'); got 'foo'"}`.
- Missing `mos` column →
  `{"error": "<path> has no 'mos' column — can't score correlations"}`.
- Missing all feature columns →
  `{"error": "... has none of the expected feature columns ..."}`.
- Fewer than 2 samples in the chosen split →
  `{"error": "split 'test' has N samples — need ≥2 to compute correlations"}`.
- Model output shape ≠ target shape →
  `{"error": "model output shape ... does not match target shape ..."}`.
- `eval` extra not installed →
  `{"error": "eval_model_on_split requires the 'eval' extra: pip install 'vmaf-mcp[eval]'"}`.

## `compare_models`

Rank several ONNX models on the same parquet split by descending PLCC.
Models that fail to load or score are collected under `errors` instead
of aborting the whole call — so the agent can surface partial results.

### Input schema

| Field        | Type                                                         | Required | Default      |
| --- | --- | --- | --- |
| `models`     | array of string (paths to `.onnx`), `minItems: 1`            | yes      | —            |
| `features`   | string (path to `.parquet`)                                  | yes      | —            |
| `split`      | `"train" \| "val" \| "test" \| "all"`                        | no       | `"test"`     |
| `input_name` | string                                                       | no       | `"features"` |

### Response body

```json
{
  "ranked": [
    { "model": "/.../baseline_v3.onnx",  "plcc": 0.9743, "srocc": 0.9612, "rmse": 3.21, "n": 137, "split": "test", "columns": [ "..." ] },
    { "model": "/.../baseline_v2.onnx",  "plcc": 0.9611, "srocc": 0.9503, "rmse": 3.80, "n": 137, "split": "test", "columns": [ "..." ] }
  ],
  "errors": [
    { "model": "/.../broken.onnx", "error": "model output shape (137, 2) does not match target shape (137,)" }
  ]
}
```

`ranked` is sorted descending by `plcc`. `errors` preserves the input
order for models that failed, with the raised exception serialised as
a string.

### Errors

- Empty or non-list `models` →
  `{"error": "'models' must be a non-empty list of paths"}`.
- Individual model failures show up under the `errors` array, not as a
  top-level error.

## `describe_worst_frames`

Score a `(ref, dis)` pair, pick the N frames with lowest VMAF, extract
each as PNG via `ffmpeg`, and describe the visible artefacts with a local
vision-language model through ONNX Runtime GenAI. Without a model it
returns the frame metadata with a note. It is a debugging aid for an LLM
agent that wants narrative context for low-quality regions. Added in
[ADR-0172](../adr/0172-mcp-describe-worst-frames.md); the model runtime
is [ADR-1886](../adr/1886-torch-training-environments-only.md).

### Setting up descriptions

The Python server only (the Go server always returns metadata):

1. `pip install 'vmaf-mcp[vlm]'` installs `onnxruntime-genai`.
2. Put an ONNX Runtime GenAI vision model on the machine, a directory
   with `genai_config.json`, the ONNX graphs and the tokenizer. The
   tested one is the CPU build of
   [Phi-3.5-vision-instruct-onnx](https://huggingface.co/microsoft/Phi-3.5-vision-instruct-onnx)
   (MIT, 3.2 GB), directory `cpu_and_mobile/cpu-int4-rtn-block-32-acc-level-4`.
   Verify the download against the SHA-256 the hub lists for each file.
   Other families ONNX Runtime GenAI supports (Phi-4 multimodal, the Qwen
   vision models, LFM2-VL, Mistral 3, Gemma 3) get their image placeholder
   from `vmaf_mcp/vlm.py`.
3. Start the server with `VMAF_MCP_VLM_MODEL=<that directory>`.

The server never downloads a model and runs no model code; it reads the
directory once, on the first call. With the tested model, a description
took 48 s per frame on four CPU cores and peaked at 8.6 GB of memory.

### Input schema

| Field      | Type                                                | Required | Default                  |
| --- | --- | --- | --- |
| `ref`      | string (path to reference YUV)                      | yes      | —                        |
| `dis`      | string (path to distorted YUV)                      | yes      | —                        |
| `width`    | integer                                             | yes      | —                        |
| `height`   | integer                                             | yes      | —                        |
| `pixfmt`   | `"420"` / `"422"` / `"444"`                         | yes      | —                        |
| `bitdepth` | 8 / 10 / 12 / 16                                    | yes      | —                        |
| `model`    | string                                              | no       | `"version=vmaf_v0.6.1"`  |
| `backend`  | `"auto"` / `"cpu"` / `"cuda"` / `"sycl"` / `"hip"` / `"metal"` | no       | `"auto"`                 |
| `n`        | integer in `[1, 32]`                                | no       | `5`                      |

### Behaviour

1. Run `vmaf_score` to populate per-frame VMAF.
2. Pick the `n` frames with smallest VMAF.
3. For each picked frame, run `ffmpeg -f rawvideo` with
   `select='eq(n,<idx>)'` and `-fps_mode passthrough` to emit a single
   PNG. This needs FFmpeg 5.1 or newer: FFmpeg 5.0 and older do not know
   `-fps_mode`, and FFmpeg 9 removed the older `-vsync 0` spelling.
4. Describe the PNG with the configured model (greedy decoding, at most
   4096 tokens including the prompt). When the `vlm` extra is missing,
   `VMAF_MCP_VLM_MODEL` is unset, or the directory has no
   `genai_config.json`, every frame's `description` is a note that says
   which, for example `(VLM unavailable: VMAF_MCP_VLM_MODEL is not set. ...)`.
5. Return frame metadata + descriptions.

The PNGs are written to a temporary directory of the call, which is
removed when the call returns; the `png` paths in the response name
where each frame was, not files that still exist.

### Response body

```json
{
  "model_id": "cpu-int4-rtn-block-32-acc-level-4",
  "frames": [
    {
      "frame_index": 12,
      "vmaf": 38.4,
      "png": "/tmp/vmaf-mcp-worst-12345/frame_000012.png",
      "description": "Heavy DCT blocking on the face and ringing along the chin contour."
    }
  ]
}
```

`model_id` is the model directory's name, and `null` when no model ran.

### Errors

- `ffmpeg` not on PATH →
  `{"error": "ffmpeg not on PATH; install ffmpeg to use describe_worst_frames"}`.
- `ffmpeg` older than 5.1 → the Python server fails the call with
  `ffmpeg frame-extract failed: ... Unrecognized option 'fps_mode'`; the Go
  server (`vmafx-mcp`) returns the frame with an empty `png` and
  `(frame extraction failed: ...)` as its `description`.
- Unsupported `pixfmt`/`bitdepth` combo →
  `{"error": "unsupported pixfmt/bitdepth combo: ..."}`.
- VMAF subprocess failure → bubbles up the underlying `vmaf_score` error.
- A configured model that fails to load or to describe a frame fails
  the call with that error; nothing falls back to metadata silently.

## `probe_backend`

Run a 1-frame VMAF health check to distinguish "compiled in" from "driver
present and functional". Uses a tiny 64×64 mid-grey synthetic YUV pair so
no fixture files are required. The frame is 64×64 (not smaller) because the
CUDA ADM kernel requires at least 36px in each dimension; a sub-36px frame
silently returns a null score on a CUDA build, which would otherwise be
misreported as `runtime_healthy: true`. A null or non-finite score now sets
`runtime_healthy: false` with an explanatory `error` string. Added in
[ADR-0634](../adr/0634-mcp-p0-iserror-and-probe-version-encoded.md).

### Input schema

| Field     | Type                                                          | Required | Notes                        |
| --- | --- | --- | --- |
| `backend` | `"cpu" \| "cuda" \| "sycl" \| "hip" \| "metal"` | yes      | Backend to health-check      |

### Response body

```json
{
  "backend":         "cuda",
  "compiled_in":     true,
  "runtime_healthy": true,
  "latency_ms":      312.5,
  "score":           100.0,
  "error":           null
}
```

- `compiled_in` — whether `vmaf --list-backends` reports the backend usable
  (same as `list_backends`).
- `runtime_healthy` — `true` iff the 1-frame score subprocess exits 0 and
  returns a finite score; `false` on driver errors, ICD missing, KFD ioctl
  failure, etc.
- `latency_ms` — wall time for the subprocess in milliseconds; `null` when
  the subprocess could not be launched.
- `score` — VMAF mean on the synthetic pair (always near 100 for identical
  ref/dis); `null` on failure.
- `error` — human-readable failure reason; `null` on success.

### Errors

All failure conditions are reported as `runtime_healthy: false` with the
reason in the `error` field — the tool itself does not raise (a non-healthy
backend is a valid result, not a protocol error).

---

## `vmaf_version`

Return the local `vmaf` binary's identity and build flags. Useful for
confirming which fork build is running before scoring. Added in
[ADR-0634](../adr/0634-mcp-p0-iserror-and-probe-version-encoded.md).

### Input schema — no arguments.

### Response body

```json
{
  "binary_path": "/usr/local/bin/vmaf",
  "version":     "1.0.0-rc.2",
  "build_flags": {
    "cpu":    true,
    "cuda":   true,
    "sycl":   false,
    "hip":    false,
    "metal":  false
  },
  "error": null
}
```

- `version` — extracted from `vmaf --version` output; `null` if the banner
  cannot be parsed or the subprocess times out.
- `build_flags` — derived from `vmaf --list-backends`, same probe as
  `list_backends`.
- `error` — set when the binary does not exist; `null` on success.

### Errors

- vmaf binary missing → `error` field is populated; all `build_flags` are
  `false`.

---

## `vmaf_score_encoded`

Score a `(reference, distorted)` pair of **encoded video files** (MP4, MKV,
Y4M, WebM, etc.) by decoding them to raw YUV via `ffmpeg`, then scoring
with the standard `vmaf_score` pipeline. Geometry (width, height, pixel
format, bit depth) is probed automatically from the reference stream — no
manual size entry required. Added in
[ADR-0634](../adr/0634-mcp-p0-iserror-and-probe-version-encoded.md).

Requires `ffmpeg` and `ffprobe` on `PATH`.

### Input schema

| Field                | Type                                                            | Required | Default                  | Notes                                          |
| --- | --- | --- | --- | --- |
| `reference_encoded`  | string (path)                                                   | yes      | —                        | Reference encoded video; must be under an allowlisted root |
| `distorted_encoded`  | string (path)                                                   | yes      | —                        | Distorted encoded video; same allowlist        |
| `model`              | string                                                          | no       | `"version=vmaf_v0.6.1"`  | Any `--model` grammar from the CLI             |
| `backend`            | `"auto" \| "cpu" \| "cuda" \| "sycl" \| "hip" \| "metal"` | no       | `"auto"`        | Backend selection                              |
| `subsample`          | integer `≥ 1`                                                   | no       | `1`                      | Score every Nth frame (1 = every frame)        |
| `precision`          | string                                                          | no       | `"legacy"`               | Passed to `--precision`                        |

All [optional scoring parameters](#optional-scoring-parameters) accepted
by `vmaf_score` (the `tiny_*` / `no_reference` tiny-AI surface,
`feature`, `aom_ctc`/`nflx_ctc`, and the frame-range controls) are also
accepted here and forwarded to the underlying `vmaf` run (ADR-1117).

### Behaviour

1. `ffprobe` the reference stream to detect width, height, pixel format, and
   bit depth.
2. Decode both reference and distorted in parallel to temp raw YUV files via
   `ffmpeg -f rawvideo`.
3. Call `_run_vmaf_score` with the decoded YUV pair and the probed geometry.
4. Inject `reference_encoded` and `distorted_encoded` keys into the response
   (the original encoded paths) alongside the full `vmaf_score` payload.

Decoded YUV temp files are automatically cleaned up after scoring via a
`TemporaryDirectory` context manager.

### Response body

Same shape as `vmaf_score`, plus two extra keys:

```json
{
  "version": "1.0.0-rc.2",
  "pooled_metrics": { "vmaf": { "mean": 76.667831, "...": "..." } },
  "frames": [ "..." ],
  "backend_requested": "auto",
  "backend_used":      "cpu",
  "reference_encoded": "/data/corpus/ref.mp4",
  "distorted_encoded": "/data/corpus/dis_crf28.mp4"
}
```

### Errors

- `ffprobe` not on PATH → raises `RuntimeError`.
- `ffmpeg` not on PATH → raises `RuntimeError`.
- No video stream in input → raises `ValueError`.
- Unknown pixel format → raises `ValueError` (e.g. `bgr24` is not mappable).
- `ffmpeg` decode failure → raises `RuntimeError` with the ffmpeg stderr.
- Same errors as `vmaf_score` for the scoring step.

---

## `list_extractors`

Enumerate all `VmafFeatureExtractor` implementations found in the local
`core/src/feature/` C source tree.  No binary required — the server
parses the C source directly. Added in
[ADR-0638](../adr/0638-mcp-p1-vmaftune-extractors-models-progress.md).

### Input schema — no arguments.

### Response body

```json
{
  "extractors": [
    { "name": "float_adm",        "backend": "cpu",    "source": "core/src/feature/float_adm.c" },
    { "name": "float_vif",        "backend": "cpu",    "source": "core/src/feature/float_vif.c" },
    { "name": "float_ssim_hip",   "backend": "hip",    "source": "core/src/feature/hip/float_ssim_hip.c" }
  ]
}
```

`name` is the string the extractor registers as its canonical identifier.
`backend` is inferred from the symbol-name suffix (`_cuda`, `_sycl`,
`_hip`, `_metal`; everything else is `cpu`).

### Errors — none (returns `{"extractors": []}` if the source tree is absent).

## `describe_model`

Return metadata for a VMAF model by name or path.  Fixes the `Path.stem` bug
(ADR-0638): `vmaf_v0.6.1` is matched against `vmaf_v0.6.1.json` correctly —
not incorrectly trimmed to `vmaf_v0.6` as Python's `Path.stem` would do.

### Input schema

| Field  | Type   | Required | Notes |
| --- | --- | --- | --- |
| `name` | string | yes      | Model stem (`vmaf_v0.6.1`), full filename (`vmaf_v0.6.1.json`), or repo-relative path. |

### Response body

```json
{
  "name":          "vmaf_v0.6.1",
  "path":          "model/vmaf_v0.6.1.json",
  "format":        "json",
  "size_bytes":    9128,
  "model_type":    "LIBSVMNUSVR",
  "feature_names": ["VMAF_integer_feature_adm2_score", "..."]
}
```

`model_type` and `feature_names` are populated for JSON models; both are
`null` for `.pkl` and `.onnx` files.

### Errors

- Unknown name →
  `{"error": "model 'foo' not found; run list_models to see available models."}`.
- Ambiguous name (two models with the same stem in different subdirs) →
  `{"error": "model name '...' is ambiguous; matched: [...]. Pass an explicit path instead."}`.

## `run_compare`

Wrap `vmaf-tune compare`: compare codec adapters at one or more target VMAF
scores and return a ranked report.  Requires `vmaf-tune` to be installed
(`pip install -e tools/vmaf-tune` or set `VMAF_TUNE_BIN`).  Emits MCP progress
notifications when `params._meta.progressToken` is set.  ADR-0638.

### Input schema

| Field          | Type    | Required | Default                              | Notes |
| --- | --- | --- | --- | --- |
| `src`          | string  | yes      | —                                    | Source video (any FFmpeg-readable format or raw YUV). |
| `target_vmaf`  | number  | no       | —                                    | Single VMAF target (legacy single-target schema). |
| `target_vmafs` | string  | no       | `"94,96,97,98"`                      | Comma-separated VMAF targets (multi-target schema). |
| `encoders`     | string  | no       | `"libx264,libx265,libsvtav1,libvpx-vp9"` | Comma-separated encoder list. |
| `width`        | integer | no       | —                                    | Source width (raw YUV only). |
| `height`       | integer | no       | —                                    | Source height (raw YUV only). |
| `pix_fmt`      | string  | no       | `"yuv420p"`                          | Source pixel format. |
| `framerate`    | number  | no       | —                                    | Source framerate. |
| `no_parallel`  | boolean | no       | `false`                              | Dispatch encoders sequentially. |

### Response body

Returns the parsed JSON output of `vmaf-tune compare --format json`.
Shape is the v1 (single-target) or v2 (multi-target) schema from ADR-0513.

### Errors

- vmaf-tune binary missing →
  `{"error": "vmaf-tune binary not found at ...; Install with: pip install -e tools/vmaf-tune or set VMAF_TUNE_BIN."}`.
- Non-zero exit → `{"error": "vmaf-tune compare exited <rc>: <stderr>"}`.

## `run_ladder`

Wrap `vmaf-tune ladder`: build a per-title bitrate ladder via convex-hull sweep
and emit an HLS / DASH / JSON manifest.  Requires `vmaf-tune`.  Emits progress
notifications.  ADR-0638.

### Input schema

| Field            | Type    | Required | Default        | Notes |
| --- | --- | --- | --- | --- |
| `src`            | string  | yes      | —              | Source video path. |
| `resolutions`    | string  | yes      | —              | Comma-separated `WxH` list, e.g. `"1920x1080,1280x720,854x480"`. |
| `target_vmafs`   | string  | yes      | —              | Comma-separated VMAF targets, e.g. `"95,90,85"`. |
| `encoder`        | string  | no       | `"libx264"`    | Codec adapter. |
| `quality_tiers`  | integer | no       | `5`            | Number of ladder rungs to select. |
| `format`         | string  | no       | `"json"`       | `"hls"`, `"dash"`, or `"json"`. |
| `spacing`        | string  | no       | `"log_bitrate"` | Knee-spacing strategy. |
| `framerate`      | number  | no       | —              | Source framerate. |

### Response body

```json
{ "manifest": { "rungs": [ ... ] }, "format": "json" }
```

For `format="hls"` or `"dash"`, `manifest` is a raw string (the M3U8 / MPD
text).

### Errors — same pattern as `run_compare`.

## `run_tune_per_shot`

Wrap `vmaf-tune tune-per-shot`: detect scene cuts and return per-shot CRF
recommendations targeting a VMAF score.  Requires `vmaf-tune`.  Emits progress
notifications.  ADR-0638.

### Input schema

| Field              | Type    | Required | Default     | Notes |
| --- | --- | --- | --- | --- |
| `src`              | string  | yes      | —           | Source video path. |
| `target_vmaf`      | number  | no       | `92.0`      | Target VMAF score. |
| `encoder`          | string  | no       | `"libx264"` | Codec adapter. |
| `pix_fmt`          | string  | no       | `"yuv420p"` | Source pixel format. |
| `framerate`        | number  | no       | —           | Source framerate. |
| `scene_threshold`  | number  | no       | —           | Scene-cut detection threshold (0..1). |
| `output`           | string  | no       | —           | Output video path (plan-only if omitted). |
| `format`           | string  | no       | `"json"`    | `"json"`, `"shell"`, or `"csv"`. |

### Response body

Returns the parsed JSON output of `vmaf-tune tune-per-shot --format json`
(list of per-shot recommendations) or the raw shell/CSV string for other
formats.

### Errors — same pattern as `run_compare`.

---

## Cross-tool error conventions

Since [ADR-0634](../adr/0634-mcp-p0-iserror-and-probe-version-encoded.md):

- All tool handler exceptions are raised, not caught and returned as
  `TextContent({"error": ...})`.
- The `mcp` library's outer handler (`_make_error_result`) therefore sets
  `isError=True` on the `CallToolResult`, and conformant clients (which
  branch on `result.isError`) treat tool errors as errors.
- Before ADR-0634 `isError` stayed implicitly `False`, so clients
  misclassified errors as successes.

| Situation                             | MCP-level behavior                                       |
| --- | --- |
| Unknown tool name                     | Raises `ValueError`; mcp sets `isError=True`            |
| Path outside allowlist                | Raises `ValueError`; mcp sets `isError=True`            |
| Path does not exist                   | Raises `FileNotFoundError`; mcp sets `isError=True`     |
| Subprocess non-zero (`vmaf_score`)    | Raises `RuntimeError`; mcp sets `isError=True`          |
| Missing optional extras               | Raises `RuntimeError`; mcp sets `isError=True`          |
| `probe_backend` unhealthy backend     | Returns success result with `runtime_healthy: false`    |

## Sidecar-binary tools

Four tools bridge the CLI binaries meson builds next to `vmaf` in
[core/tools/](../../core/tools/). Both servers implement them and build a
byte-identical argv (pinned by `cmd/vmafx-mcp/sidecar_parity_test.go`).

Every numeric and enum bound below is the bound the corresponding C parser
enforces, so an out-of-range value fails with a readable MCP error instead of a
`usage()` dump on stderr.

### Binary resolution

Each tool resolves its binary in this order, and the first hit wins:

1. the tool's own environment override,
2. a **sibling of the resolved `vmaf` binary** — so `VMAF_BIN` resolves the
   whole
   family, which is what the `vmaf-dev-mcp` container relies on after
   `make install`,
3. `/usr/local/bin/<name>`,
4. `<repo>/core/build/tools/<name>`,
5. `<repo>/build/tools/<name>`.

| Tool | Binary | Environment override |
| --- | --- | --- |
| `vmaf_per_shot` | `vmaf-perShot` | `VMAF_PER_SHOT_BIN` |
| `vmaf_roi` | `vmaf_roi` | `VMAF_ROI_BIN` |
| `vmaf_bench` | `vmaf_bench` | `VMAF_BENCH_BIN` |
| `vmaf_vpl` | `vmaf_vpl` | `VMAF_VPL_BIN` |

When none of the candidates exists the tool returns `isError=true` naming both
the path it looked for and the `meson compile` target that builds it.

## `vmaf_per_shot`

Wrap [`vmaf-perShot`](../usage/vmaf-perShot.md): scan a raw YUV reference,
detect shot boundaries from luma complexity + motion energy, and return a
per-shot CRF plan targeting a VMAF score. ADR-0222.

### Input schema

| Field | Type | Required | Default | Notes |
| --- | --- | --- | --- | --- |
| `reference` | string (path) | yes | — | Raw planar YUV; must be under an allowed root. |
| `width` | integer `16..65535` | yes | — | Frame width. |
| `height` | integer `16..65535` | yes | — | Frame height. |
| `pixel_format` | `"420" \| "422" \| "444"` | no | `"420"` | `--pixel_format`. |
| `bitdepth` | `8 \| 10 \| 12 \| 16` | no | `8` | `--bitdepth`. |
| `target_vmaf` | number `0..100` | no | `90` | `--target-vmaf`. |
| `crf_min` | integer `0..63` | no | `18` | `--crf-min`; must not exceed `crf_max`. |
| `crf_max` | integer `0..63` | no | `35` | `--crf-max`. |
| `diff_threshold` | number `0..255` | no | — | `--diff-threshold`. Omitted from argv when unset, so the C default (12.0) stays authoritative. |
| `format` | `"json" \| "csv"` | no | `"json"` | `--format`. The MCP tool defaults to `json` where the C CLI defaults to `csv`, so the plan comes back structured. |

### Response body

```json
{
  "format": "json",
  "exit_code": 0,
  "stderr": "vmaf-perShot: wrote 2 shot(s) to -\n",
  "plan": {
    "target_vmaf": 92.0,
    "crf_min": 18,
    "crf_max": 35,
    "shots": [
      {"shot_id": 0, "start_frame": 0, "end_frame": 3, "frames": 4,
       "mean_complexity": 0.000051, "mean_motion": 0.020046, "predicted_crf": 25.31}
    ]
  }
}
```

With `format="csv"` the `plan` key is replaced by `output`, the raw CSV text.

### Errors

`isError=true` on: a `reference` outside the allowlist, any out-of-range
argument, `crf_min > crf_max`, a missing binary, a non-zero exit
(`vmaf-perShot exited N: <stderr>`), or JSON the plan writer produced that does
not parse.

## `vmaf_roi`

Wrap `vmaf_roi`: compute a per-CTU saliency grid for **one** frame of a raw YUV
file and emit an encoder ROI sidecar.

### Input schema

| Field | Type | Required | Default | Notes |
| --- | --- | --- | --- | --- |
| `reference` | string (path) | yes | — | Raw planar YUV, under an allowed root. |
| `width` | integer `1..16384` | yes | — | Frame width. |
| `height` | integer `1..16384` | yes | — | Frame height. |
| `frame` | integer `0..1000000` | yes | — | 0-based frame index (`--frame`). |
| `pixel_format` | `"420" \| "422" \| "444"` | no | `"420"` | |
| `bitdepth` | `8 \| 10 \| 12 \| 16` | no | `8` | |
| `ctu_size` | integer `8..128` | no | `64` | `--ctu-size` (x265 max-ctu). |
| `encoder` | `"x265" \| "svt-av1"` | no | `"x265"` | Sidecar dialect. |
| `strength` | number `0..64` | no | `6.0` | QP-offset gain (`--strength`). |
| `saliency_model` | string (path) | no | — | ONNX `[1,1,H,W]` luma→`[0,1]` model, under an allowed root. Without it a centre-weighted radial placeholder is used — smoke-test quality only. |

### Response body

The sidecar is always written to a server-owned temp file, never to stdout: the
SVT-AV1 emitter produces a raw `int8` grid that is not text-safe. The response
carries the bytes in the encoding that matches the emitter.

```json
{
  "encoder": "x265",
  "sidecar_fmt": "qpfile",
  "exit_code": 0,
  "stderr": "",
  "ctu_size": 64,
  "frame": 2,
  "bytes": 216,
  "grid_cols": 9,
  "grid_rows": 6,
  "saliency": "placeholder",
  "qpfile": "# vmaf-roi qpfile (x265, --qpfile-style)\n# frame=2 ctu=64 cols=9 rows=6 strength=6.000\n4 2 1 -1 -1 -1 1 2 4\n…"
}
```

For `encoder="svt-av1"`, `sidecar_fmt` is `"roi_map_int8"` and `qpfile` is
replaced by `roi_map_base64` — the base64 of the raw row-major `int8` grid.
`saliency` is `"onnx"` when a `saliency_model` was supplied, `"placeholder"`
otherwise.

### Errors — same pattern as `vmaf_per_shot`.

## `vmaf_bench`

Wrap `vmaf_bench`: a per-feature micro-benchmark over the built-in synthetic
fixtures, or a GPU-vs-CPU correctness comparison.

This is **not** [`run_benchmark`](#run_benchmark): that one runs the end-to-end
`bench_all.sh` harness over real YUV fixtures across every backend and takes no
arguments (ADR-0513 / ADR-0517). `vmaf_bench` is the per-feature timing tool.

### Input schema

| Field | Type | Required | Default | Notes |
| --- | --- | --- | --- | --- |
| `frames` | integer `2..48` | no | — | `--frames`; the C default is 10 and 48 is `MAX_TEST_FRAMES`. Out-of-range values are rejected rather than silently clamped as the C parser does. |
| `resolution` | `"576x324" \| "640x480" \| "1280x720" \| "1920x1080" \| "3840x2160"` | no | — | `--resolution`. Omit to test all five. |
| `bpc` | `8 \| 10 \| 12 \| 16` | no | — | `--bpc`; C default 8. |
| `data_dir` | string (path) | no | — | `--data-dir`. Must be a **directory** under an allowed root. Otherwise `VMAF_TEST_DATA` applies. |
| `validate` | boolean | no | `false` | `--validate`: compare GPU vs CPU scores. |
| `gpu_only` | boolean | no | `false` | `--gpu-only`: skip the CPU targets. |
| `device_list` | boolean | no | `false` | `--list-devices`: list GPU devices and exit. |

### Response body

```json
{
  "mode": "benchmark",
  "exit_code": 0,
  "stdout": "VMAF Performance Benchmark (…)\nFeature   Res   Init ms   Avg ms …",
  "stderr": ""
}
```

In `validate` mode the payload additionally carries `"validation_failed":
true|false` and `"mode": "validate"`.

### Errors

In **benchmark** mode a non-zero exit is a tool error. In **validate** mode it
is
not: `vmaf_bench --validate` exits 1 to report that the GPU/CPU comparison found
deltas, which is a result rather than a failure, so the call succeeds with
`validation_failed=true`. Argument validation errors and a missing binary are
always tool errors.

## `vmaf_vpl`

Wrap `vmaf_vpl`: decode an encoded `(reference, distorted)` pair with Intel
oneVPL, import the VA surfaces zero-copy into SYCL over DMA-BUF, and score them.

The binary is only built when the oneVPL + libva + SYCL toolchain is present. On
a build without it the tool returns `isError=true` naming the missing binary —
it never silently falls back to another path.

### Input schema

| Field | Type | Required | Default | Notes |
| --- | --- | --- | --- | --- |
| `ref` | string (path) | yes | — | Reference encoded video, under an allowed root. |
| `dis` | string (path) | yes | — | Distorted encoded video. |
| `model` | string | no | `"vmaf_v0.6.1"` | `--model`. A **bare model name**; a value containing `/`, `\`, a space or a tab is rejected. The default mirrors the sidecar's own. |
| `frames` | integer `≥ 0` | no | `0` | `--frames`; 0 = all. |
| `device` | integer `≥ 0` | no | `0` | `--device` (SYCL device index). |
| `render_node` | string | no | `"/dev/dri/renderD128"` | `--render-node`. Restricted to `/dev/dri/renderD<N>` or `/dev/dri/card<N>` — the value goes straight to `open(2)`, so anything else is rejected. |
| `fallback` | boolean | no | `false` | `--fallback`: host upload when the zero-copy import fails. |

### Response body

```json
{
  "exit_code": 0,
  "stdout": "Frames: 12\nTime:   0.400 s (30.0 FPS)\nVMAF:   96.123456 (mean)\n…",
  "stderr": "",
  "model": "vmaf_v0.6.1",
  "device": 0,
  "render_node": "/dev/dri/renderD128",
  "vmaf_score": 96.123456,
  "frames_processed": 12
}
```

`vmaf_score` and `frames_processed` are parsed out of the sidecar's summary
lines; both are omitted when the sidecar did not print them (for example when
the model failed to load).

## Control-plane tools (gRPC bridge, **Go only**)

Five tools expose the Phase-4b control plane: the `vmafx-controller` job API and
the `vmafx-server` scoring API. They exist **only in the Go server**
(`vmafx-mcp`); the Python server does not ship a gRPC stack. See
[ADR-1184](../adr/1184-mcp-grpc-bridge-go-only.md) for why, and
[docs/architecture/phase4b-distributed-platform.md](../architecture/phase4b-distributed-platform.md)
for the topology.

### Configuration

Connection targets are **environment-only** — a tool argument naming a host
would
turn the MCP server into an SSRF pivot.

| Variable | Default | Meaning |
| --- | --- | --- |
| `VMAFX_CONTROLLER_ADDR` | `localhost:9090` | `vmafx-controller` gRPC address. |
| `VMAFX_SERVER_ADDR` | `localhost:9090` | `vmafx-server` gRPC address (used by `vmaf_score_remote`). |
| `VMAFX_CONTROLLER_TOKEN` | — | Optional bearer token; sent as `authorization: Bearer <token>` on every controller RPC. Omit it against a controller started with `VMAFX_AUTH_DISABLED=true`. With auth enabled the token needs `vmafx:reader` for `get_job` and `list_jobs`, and `vmafx:writer` for `submit_job` and `cancel_job` ([roles](../server/auth.md#roles-and-rbac)). |
| `VMAFX_GRPC_TIMEOUT` | `30` | Per-RPC deadline in seconds. A malformed or non-positive value falls back to the default rather than disabling the deadline. |

The transport is insecure by default, matching `pkg/score.Dial`: the control
plane is expected to run inside the cluster mesh.

### Path arguments are resolved remotely

`reference` / `distorted` on these tools are **not** checked against the MCP
host's allowlist — the file lives on the worker node's shared mount, so
`VMAF_MCP_ALLOW` does not apply and the file need not exist locally. What is
enforced is the shape: the value must be absolute, must contain no `..`
component, and must contain no NUL / CR / LF.

### `submit_job`

Enqueue a scoring job and return its ID.

| Field | Type | Required | Default | Notes |
| --- | --- | --- | --- | --- |
| `reference` | string (absolute worker-side path) | yes | — | |
| `distorted` | string (absolute worker-side path) | yes | — | |
| `model` | string | no | — | Omit to let the controller apply its own default. |
| `backend` | `"auto" \| "cpu" \| "cuda" \| "sycl" \| "hip" \| "metal"` | no | `"auto"` | Node capability the scheduler must match. `auto` is spelled as the empty string on the wire. |

```json
{
  "job_id": "555090fd-1349-471f-8a07-751f37e162cc",
  "status": "PENDING",
  "controller": "127.0.0.1:9090",
  "scoring": {"reference": "/mnt/data/ref.yuv", "distorted": "/mnt/data/dis.yuv",
              "model": "", "backend": "cpu"}
}
```

### `get_job`

Fetch a job by ID (`job_id`, required).

```json
{
  "id": "555090fd-…", "status": "CANCELLED", "assigned_node": "", "error": "",
  "created_at": 1788622781, "updated_at": 1788622781, "final_score": 0,
  "scoring": {"reference": "…", "distorted": "…", "model": "", "backend": "cpu"},
  "controller": "127.0.0.1:9090"
}
```

`status` is one of `PENDING`, `RUNNING`, `COMPLETED`, `FAILED`, `CANCELLED`.
`partial_results` and `partial_result_count` appear only when the node has
reported per-frame partials. An unknown ID is a tool error carrying the
controller's `NotFound` status.

### `cancel_job`

Request cancellation of a PENDING or RUNNING job (`job_id`, required). Returns
`{"job_id": …, "ok": true, "message": "cancellation requested", "controller": …}`.
A running job's node stops it within one heartbeat interval (10 s by default,
[controller guide](../server/controller.md#cancel-a-job)).
`ok=false` means the controller declined; the `message` says why.

### `list_jobs`

List the controller's current job snapshot: the jobs of the token's tenant
(`VMAFX_CONTROLLER_TOKEN`), or of the `dev` tenant against a controller with
auth disabled.

| Field | Type | Required | Default | Notes |
| --- | --- | --- | --- | --- |
| `status_filter` | array of status strings | no | — | Any of `PENDING`, `RUNNING`, `COMPLETED`, `FAILED`, `CANCELLED`; case-insensitive. Omit for all jobs. |
| `limit` | integer `1..500` | no | `100` | Maximum jobs returned. |

```json
{"jobs": [ … ], "count": 1, "truncated": false, "limit": 100,
 "controller": "127.0.0.1:9090"}
```

Each entry has the `get_job` shape. The tool drains the `StreamJobs`
server-streaming RPC, which sends the current snapshot and closes (ADR-0962) —
it is not a subscription. `truncated=true` means the controller had more of the
tenant's jobs than `limit`.

### `vmaf_score_remote`

Score a pair on a remote `vmafx-server` over the unary `VmafxScoring.Score` RPC.
Nothing is read locally.

| Field | Type | Required | Default | Notes |
| --- | --- | --- | --- | --- |
| `reference` | string (absolute server-side path) | yes | — | |
| `distorted` | string (absolute server-side path) | yes | — | |
| `model` | string | no | — | Omit to let the server apply its own default. |

```json
{"score": 76.6683, "features": {"vif_scale0": 0.79, "adm2": 0.93},
 "model": "", "reference": "/mnt/data/ref.yuv", "distorted": "/mnt/data/dis.yuv",
 "server": "127.0.0.1:9090"}
```

Use [`vmaf_score`](#vmaf_score) for files on the MCP host.

## Progress notifications

All four `run_*` tools — `run_benchmark`, `run_compare`, `run_ladder`, and
`run_tune_per_shot` — emit `notifications/progress` when the client supplies a
`progressToken` in the request's `_meta` object:

```json
{
  "method": "tools/call",
  "params": {
    "name": "run_compare",
    "arguments": { "src": "/path/to/video.mp4" },
    "_meta": { "progressToken": "my-token-42" }
  }
}
```

The server sends two progress events per tool call:

| Event      | `progress` | `total` | `message` |
| --- | --- | --- | --- |
| Start      | `0.0`     | `1.0`   | `"starting vmaf-tune compare"` (tool-specific) |
| Completion | `1.0`     | `1.0`   | `"vmaf-tune compare done"` |

No finer-grained progress is available because the tools delegate to a
subprocess.
Clients without a token receive no progress events (per MCP spec — the server
must not send unsolicited progress).

- [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md).

## Related

- [MCP server overview](index.md) — install, security model, env vars.
- [CLI reference](../usage/cli.md) — the CLI that `vmaf_score` wraps.
- [`vmaf_bench`](../usage/bench.md) — what `run_benchmark` drives.
- [Tiny-AI inference](../ai/inference.md) — what
  `eval_model_on_split` / `compare_models` are scoring.
- [ADR-0634](../adr/0634-mcp-p0-iserror-and-probe-version-encoded.md) — P0
  fixes.
- [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md).
