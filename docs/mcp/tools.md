<!-- markdownlint-disable MD013 MD024 MD026 MD060 -->
# MCP tool reference

Per-tool request / response schemas and error semantics for
[`vmaf-mcp`](index.md). Source of truth: the `list_tools()` handler in
[mcp-server/vmaf-mcp/src/vmaf_mcp/server.py](../../mcp-server/vmaf-mcp/src/vmaf_mcp/server.py).

Every tool returns a single `TextContent` message whose body is a JSON
document. On error the body has shape `{"error": "<string>"}`, so clients
can `json.loads()` unconditionally and branch on the presence of
`error`.

## `vmaf_score`

Score one `(ref, dis)` YUV pair and return the full VMAF JSON report.

### Input schema

| Field       | Type                                   | Required | Default                 | Notes                                          |
|-------------|----------------------------------------|----------|-------------------------|------------------------------------------------|
| `ref`       | string (path)                          | yes      | —                       | Reference YUV; must be under an allowed root   |
| `dis`       | string (path)                          | yes      | —                       | Distorted YUV; same allowlist                  |
| `width`     | integer `≥ 1`                          | yes      | —                       | Frame width in pixels                          |
| `height`    | integer `≥ 1`                          | yes      | —                       | Frame height in pixels                         |
| `pixfmt`    | `"420" \| "422" \| "444"`              | yes      | —                       | YUV chroma subsampling                         |
| `bitdepth`  | `8 \| 10 \| 12 \| 16`                  | yes      | —                       | Bit depth of both YUV files                    |
| `model`     | string                                 | no       | `"version=vmaf_v0.6.1"` | Any `--model` grammar from the CLI             |
| `backend`   | `"auto" \| "cpu" \| "cuda" \| "sycl" \| "hip" \| "metal"` | no       | `"auto"`                | Backend selection; `auto` lets vmaf pick. Requesting a backend the local binary does not advertise raises (no silent fallback — ADR-0495). |
| `subsample` | integer `≥ 1`                          | no       | `1`                     | Score every Nth frame (passed as `--subsample`) |
| `precision` | string                                 | no       | `"legacy"`              | Passed straight to `--precision` (see below)   |

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

The JSON written by vmaf is parsed and returned with two extra
fields injected by the MCP layer (ADR-0495):

- `backend_requested` — verbatim echo of the caller's `backend` arg.
- `backend_used` — what actually ran. For an explicit `backend` arg
  this equals the requested value (the wrapper refuses to silently
  fall back); for `backend="auto"` it's a best-effort label
  inferred from the JSON's per-backend key-count signature
  (`"cpu"` / `"gpu"`).

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
  "version": "3.2.1",
  "pooled_metrics": { "vmaf": { "mean": 76.668905, "...": "..." } },
  "frames": [ { "frameNum": 0, "metrics": { "vmaf": 78.8263, "...": "..." } } ]
}
```

### Errors

- Path not under an allowlisted root → `{"error": "path ... not under an allowlisted root; set VMAF_MCP_ALLOW to extend."}`.
- Path does not exist → `{"error": "<abs-path>"}` from `FileNotFoundError`.
- vmaf binary missing → `{"error": "vmaf binary not found at ...; Build first: meson compile -C build."}`.
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
|---------------------|----------------------------------------------------------------------------------------------------------------------------|-----------------------|----------------------------------------------------------------------------------------------------|
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
|------------|----------------------------------------------------------|---------------|------------------------------------------------------------------------------------------------|
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
|-------------------|---------------|---------------------|--------------------------------------------------------|
| `threads`         | integer `≥ 1` | `--threads`         | Worker threads (capped to hardware cores by the CLI).  |
| `frame_cnt`       | integer `≥ 1` | `--frame_cnt`       | Maximum number of frames to process.                   |
| `frame_skip_ref`  | integer `≥ 0` | `--frame_skip_ref`  | Skip the first N reference frames.                     |
| `frame_skip_dist` | integer `≥ 0` | `--frame_skip_dist` | Skip the first N distorted frames.                     |
| `no_prediction`   | boolean       | `--no_prediction`   | Extract features only; skip VMAF prediction.           |

#### Device selectors

Control hardware device selection and hardware capability masks on
heterogeneous systems (#1240).

| Field          | Type          | CLI flag         | Notes                                                        |
|----------------|---------------|------------------|--------------------------------------------------------------|
| `cpumask`      | integer `≥ 0` | `--cpumask`      | Bitmask restricting permitted CPU SIMD instruction sets.     |
| `gpumask`      | integer `≥ 0` | `--gpumask`      | Bitmask restricting permitted GPU operations.                |
| `sycl_device`  | integer `≥ 0` | `--sycl_device`  | Select SYCL GPU device by index.                             |
| `hip_device`   | integer `≥ 0` | `--hip_device`   | Select HIP GPU device by index.                              |
| `metal_device` | integer `≥ 0` | `--metal_device` | Select Metal GPU device by index.                            |

#### Output format

Select the serialization format emitted by the underlying `vmaf` engine (#1240).

| Field        | Type / values                       | CLI flag                           | Notes                                                              |
|--------------|-------------------------------------|------------------------------------|--------------------------------------------------------------------|
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

The server probes `vmaf --help` and looks for `--no_<backend>` flags;
`cpu` is always reported `true` when the binary exists. Note: this tool
reports **compiled-in** backends only — a backend may be compiled in but
its driver missing or non-functional at runtime. Use `probe_backend` to
verify that a backend can actually run a score.

### Errors

- If the vmaf binary is missing, every flag is `false` — no error is
  raised. Call `list_backends` before other tools to test whether the
  build is usable.

## run_benchmark

Run the full multi-fixture benchmark suite (`testdata/bench_all.sh`) against all
available compiled-in backends (CPU, CUDA, SYCL, HIP, Metal — Vulkan removed in
ADR-0726) on three canonical YUV fixture pairs built into the harness:

1. **576×324, 48 frames, 8-bit** — the Netflix golden pair `src01_hrc00 / src01_hrc01`
2. **1920×1080, 5 frames, 8-bit** — the 5-frame 1080p pair
3. **3840×2160, 200 frames, 8-bit** — the 4K BBB excerpt (`testdata/bbb/`)

For each fixture the harness scores all compiled-in backends, prints per-backend VMAF means
and wall times, and prints a comparison table showing max per-frame diff between
CPU and each GPU backend. See [usage/bench.md](../usage/bench.md) for more detail.

> **This tool does not accept per-call `ref`/`dis` arguments.** Per-pair scoring is
> the job of `vmaf_score`. `bench_all.sh` is a fixed-fixture harness. (ADR-0517)
>
> **Protocol note**: `run_benchmark` runs the full 4K test which takes 30–60 seconds
> on a modern GPU. Real MCP clients hold the connection open. The heredoc test pattern
> (`docker exec -i ... vmaf-mcp << EOF ... EOF`) causes the server to shut down on
> stdin EOF before the benchmark completes. Use a persistent pipe (`sleep 120 |`)
> when testing from the command line. See [Finding 9 in the E2E test matrix](../../.workingdir/bbb_reports/E2E_TEST_MATRIX_v9.md).
>
> **Error contract**: `run_benchmark` raises `RuntimeError("benchmark failed — no
> output line containing pooled score / Pearson correlation")` on partial / silent
> pipe failures (ADR-0638). A second legacy implementation that swallowed the
> failure and returned a partial dict was removed in PR #517 (Layer-5); MCP clients
> should now branch on `isError=True` per the [MCP error contract](../../docs/mcp/index.md).

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

The `stdout` field contains the full human-readable benchmark output. Per-backend JSON
result files are written to `/tmp/vmaf-bench-<pid>/` (or to `VMAF_BENCH_OUTDIR` if set).

### Errors

- `testdata/bench_all.sh` missing → raises `FileNotFoundError` with path.
- Non-zero `exit_code` is not itself an error field — both stdout and stderr are
  always returned so the caller can diagnose partial failures.
- Non-zero exit + empty stdout + empty stderr → `error` key is added with a
  root-cause shortlist and a `bash -x` re-run hint. Common causes: missing vmaf
  binary or missing fixture YUVs under `testdata/bbb/`.
- Unavailable backends (Vulkan without ICD, HIP scaffold-only) produce a `SKIP`
  line in stdout and do not abort the harness.

## `eval_model_on_split`

Load an ONNX tiny-AI regressor, run it against a parquet feature cache,
filter to a deterministic `train` / `val` / `test` split (keyed by the
`key` column via SHA-256 bucketing — same scheme as `vmaf_train`), and
report correlations against the `mos` target.

**Python server** (`vmaf-mcp`) requires the optional `eval` extra:

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

which pulls in `numpy`, `pandas`, `scipy`, and `onnxruntime`.

### Input schema

| Field        | Type                                                         | Required | Default      |
|--------------|--------------------------------------------------------------|----------|--------------|
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

- Bad split name → `{"error": "split must be one of ('train', 'val', 'test', 'all'); got 'foo'"}`.
- Missing `mos` column → `{"error": "<path> has no 'mos' column — can't score correlations"}`.
- Missing all feature columns → `{"error": "... has none of the expected feature columns ..."}`.
- Fewer than 2 samples in the chosen split → `{"error": "split 'test' has N samples — need ≥2 to compute correlations"}`.
- Model output shape ≠ target shape → `{"error": "model output shape ... does not match target shape ..."}`.
- `eval` extra not installed → `{"error": "eval_model_on_split requires the 'eval' extra: pip install 'vmaf-mcp[eval]'"}`.

## `compare_models`

Rank several ONNX models on the same parquet split by descending PLCC.
Models that fail to load or score are collected under `errors` instead
of aborting the whole call — so the agent can surface partial results.

### Input schema

| Field        | Type                                                         | Required | Default      |
|--------------|--------------------------------------------------------------|----------|--------------|
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

- Empty or non-list `models` → `{"error": "'models' must be a non-empty list of paths"}`.
- Individual model failures show up under the `errors` array, not as a
  top-level error.

## `describe_worst_frames`

Score a `(ref, dis)` pair, pick the N frames with lowest VMAF, extract
each as PNG via `ffmpeg`, and run a vision-language model
(SmolVLM → Moondream2 fallback) to describe the visible artefacts.
Falls back to a metadata-only output when the `vlm` extras aren't
installed — useful as a debugging affordance for an LLM agent that
wants narrative context for low-quality regions. Added in
[ADR-0172](../adr/0172-mcp-describe-worst-frames.md) (T6-6).

### Input schema

| Field      | Type                                                | Required | Default                  |
|------------|-----------------------------------------------------|----------|--------------------------|
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
   `select='eq(n,<idx>)'` to emit a single PNG.
4. Pass the PNG to the cached VLM pipeline. The pipeline is loaded
   lazily on first call:
   - Try `HuggingFaceTB/SmolVLM-Instruct` (~2 GB).
   - Fall back to `vikhyatk/moondream2` (~2 GB).
   - If neither loads (or `transformers` isn't importable), every
     frame's `description` carries
     `"(VLM unavailable — install with pip install vmaf-mcp[vlm])"`.
5. Return frame metadata + descriptions.

The PNGs are written under `/tmp/vmaf-mcp-worst-<pid>/`. They aren't
auto-deleted — callers can fetch them at the returned `png` paths
during the lifetime of the process.

### Response body

```json
{
  "model_id": "HuggingFaceTB/SmolVLM-Instruct",
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

`model_id` is `null` when the metadata-only fallback path fired.

### Errors

- `ffmpeg` not on PATH → `{"error": "ffmpeg not on PATH; install ffmpeg to use describe_worst_frames"}`.
- Unsupported `pixfmt`/`bitdepth` combo → `{"error": "unsupported pixfmt/bitdepth combo: ..."}`.
- VMAF subprocess failure → bubbles up the underlying `vmaf_score` error.
- VLM inference exception per-frame → the frame's `description`
  carries the exception string; other frames still proceed.

## `probe_backend`

Run a 1-frame VMAF health check to distinguish "compiled in" from "driver
present and functional". Uses a tiny 64×64 mid-grey synthetic YUV pair so
no fixture files are required. The frame is 64×64 (not smaller) because the
CUDA ADM kernel requires at least 36px in each dimension; a sub-36px frame
silently returns a null score on a CUDA build, which would otherwise be
misreported as `runtime_healthy: true`. A null or non-finite score now sets
`runtime_healthy: false` with an explanatory `error` string. Added in
[ADR-0613](../adr/0613-mcp-p0-iserror-and-probe-version-encoded.md).

### Input schema

| Field     | Type                                                          | Required | Notes                        |
|-----------|---------------------------------------------------------------|----------|------------------------------|
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

- `compiled_in` — whether `vmaf --help` advertises `--no_<backend>` (same
  as `list_backends`).
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
[ADR-0613](../adr/0613-mcp-p0-iserror-and-probe-version-encoded.md).

### Input schema — no arguments.

### Response body

```json
{
  "binary_path": "/usr/local/bin/vmaf",
  "version":     "3.2.1",
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
- `build_flags` — derived from `vmaf --help` (`--no_<backend>` presence),
  same probe as `list_backends`.
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
[ADR-0613](../adr/0613-mcp-p0-iserror-and-probe-version-encoded.md).

Requires `ffmpeg` and `ffprobe` on `PATH`.

### Input schema

| Field                | Type                                                            | Required | Default                  | Notes                                          |
|----------------------|-----------------------------------------------------------------|----------|--------------------------|------------------------------------------------|
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
  "version": "3.2.1",
  "pooled_metrics": { "vmaf": { "mean": 76.668905, "...": "..." } },
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

## Cross-tool error conventions

**ADR-0613 (isError spec-correctness):** From ADR-0613 onward, all tool
handler exceptions are propagated as raises rather than being caught and
returned as `TextContent({"error": ...})`. This allows the `mcp` library's
outer handler (`_make_error_result`) to set `isError=True` on the
`CallToolResult`, so conformant MCP clients (which branch on `result.isError`)
correctly treat tool errors as errors. The previous pattern left `isError`
implicitly `False`, causing clients to misclassify errors as successes.

| Situation                             | MCP-level behavior                                       |
|---------------------------------------|----------------------------------------------------------|
| Unknown tool name                     | Raises `ValueError`; mcp sets `isError=True`            |
| Path outside allowlist                | Raises `ValueError`; mcp sets `isError=True`            |
| Path does not exist                   | Raises `FileNotFoundError`; mcp sets `isError=True`     |
| Subprocess non-zero (`vmaf_score`)    | Raises `RuntimeError`; mcp sets `isError=True`          |
| Missing optional extras               | Raises `RuntimeError`; mcp sets `isError=True`          |
| `probe_backend` unhealthy backend     | Returns success result with `runtime_healthy: false`    |

## Related

- [MCP server overview](index.md) — install, security model, env vars.
- [CLI reference](../usage/cli.md) — the CLI that `vmaf_score` wraps.
- [`vmaf_bench`](../usage/bench.md) — what `run_benchmark` drives.
- [Tiny-AI inference](../ai/inference.md) — what
  `eval_model_on_split` / `compare_models` are scoring.
- [ADR-0613](../adr/0613-mcp-p0-iserror-and-probe-version-encoded.md) — P0 fixes.
- [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md).

## `list_extractors`

Enumerate all `VmafFeatureExtractor` implementations found in the local
`core/src/feature/` C source tree.  No binary required — the server
parses the C source directly.  Added in [ADR-0638](../adr/0638-mcp-p1-vmaftune-extractors-models-progress.md).

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
|--------|--------|----------|-------|
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

- Unknown name → `{"error": "model 'foo' not found; run list_models to see available models."}`.
- Ambiguous name (two models with the same stem in different subdirs) →
  `{"error": "model name '...' is ambiguous; matched: [...]. Pass an explicit path instead."}`.

## `run_compare`

Wrap `vmaf-tune compare`: compare codec adapters at one or more target VMAF
scores and return a ranked report.  Requires `vmaf-tune` to be installed
(`pip install -e tools/vmaf-tune` or set `VMAF_TUNE_BIN`).  Emits MCP progress
notifications when `params._meta.progressToken` is set.  ADR-0638.

### Input schema

| Field          | Type    | Required | Default                              | Notes |
|----------------|---------|----------|--------------------------------------|-------|
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

- vmaf-tune binary missing → `{"error": "vmaf-tune binary not found at ...; Install with: pip install -e tools/vmaf-tune or set VMAF_TUNE_BIN."}`.
- Non-zero exit → `{"error": "vmaf-tune compare exited <rc>: <stderr>"}`.

## `run_ladder`

Wrap `vmaf-tune ladder`: build a per-title bitrate ladder via convex-hull sweep
and emit an HLS / DASH / JSON manifest.  Requires `vmaf-tune`.  Emits progress
notifications.  ADR-0638.

### Input schema

| Field            | Type    | Required | Default        | Notes |
|------------------|---------|----------|----------------|-------|
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

For `format="hls"` or `"dash"`, `manifest` is a raw string (the M3U8 / MPD text).

### Errors — same pattern as `run_compare`.

## `run_tune_per_shot`

Wrap `vmaf-tune tune-per-shot`: detect scene cuts and return per-shot CRF
recommendations targeting a VMAF score.  Requires `vmaf-tune`.  Emits progress
notifications.  ADR-0638.

### Input schema

| Field              | Type    | Required | Default     | Notes |
|--------------------|---------|----------|-------------|-------|
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
(list of per-shot recommendations) or the raw shell/CSV string for other formats.

### Errors — same pattern as `run_compare`.

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
2. a **sibling of the resolved `vmaf` binary** — so `VMAF_BIN` resolves the whole
   family, which is what the `vmaf-dev-mcp` container relies on after
   `make install`,
3. `/usr/local/bin/<name>`,
4. `<repo>/core/build/tools/<name>`,
5. `<repo>/build/tools/<name>`.

| Tool | Binary | Environment override |
|---|---|---|
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
|---|---|---|---|---|
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
|---|---|---|---|---|
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
|---|---|---|---|---|
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

In **benchmark** mode a non-zero exit is a tool error. In **validate** mode it is
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
|---|---|---|---|---|
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

Connection targets are **environment-only** — a tool argument naming a host would
turn the MCP server into an SSRF pivot.

| Variable | Default | Meaning |
|---|---|---|
| `VMAFX_CONTROLLER_ADDR` | `localhost:9090` | `vmafx-controller` gRPC address. |
| `VMAFX_SERVER_ADDR` | `localhost:9090` | `vmafx-server` gRPC address (used by `vmaf_score_remote`). |
| `VMAFX_CONTROLLER_TOKEN` | — | Optional bearer token; sent as `authorization: Bearer <token>` on every controller RPC. Omit it against a controller started with `VMAFX_AUTH_DISABLED=true`. |
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
|---|---|---|---|---|
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
`ok=false` means the controller declined; the `message` says why.

### `list_jobs`

List the controller's current job snapshot.

| Field | Type | Required | Default | Notes |
|---|---|---|---|---|
| `status_filter` | array of status strings | no | — | Any of `PENDING`, `RUNNING`, `COMPLETED`, `FAILED`, `CANCELLED`; case-insensitive. Omit for all jobs. |
| `limit` | integer `1..500` | no | `100` | Maximum jobs returned. |

```json
{"jobs": [ … ], "count": 1, "truncated": false, "limit": 100,
 "controller": "127.0.0.1:9090"}
```

Each entry has the `get_job` shape. The tool drains the `StreamJobs`
server-streaming RPC, which sends the current snapshot and closes (ADR-0962) —
it is not a subscription. `truncated=true` means the controller had more jobs
than `limit`.

### `vmaf_score_remote`

Score a pair on a remote `vmafx-server` over the unary `VmafxScoring.Score` RPC.
Nothing is read locally.

| Field | Type | Required | Default | Notes |
|---|---|---|---|---|
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
|------------|-----------|---------|-----------|
| Start      | `0.0`     | `1.0`   | `"starting vmaf-tune compare"` (tool-specific) |
| Completion | `1.0`     | `1.0`   | `"vmaf-tune compare done"` |

No finer-grained progress is available because the tools delegate to a subprocess.
Clients without a token receive no progress events (per MCP spec — the server
must not send unsolicited progress).

- [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md).
