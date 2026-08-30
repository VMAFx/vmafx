<!-- markdownlint-disable MD013 MD060 -->
# vmafx-tune-go — Go port of vmaf-tune (Stage 6)

`vmafx-tune-go` is the Go port of the `vmaf-tune` rate-quality tuning CLI
(Stages 1–4 plus the Stage-6 `fast` path). It ships as a **separate binary**
alongside the Python `vmaf-tune` binary during the migration. The Python binary
is unchanged and should be used for all subcommands that are not yet ported.

This page documents the Go binary. For the full Python `vmaf-tune` reference, see
[vmaf-tune.md](vmaf-tune.md).

## Build

```bash
go build -o vmafx-tune-go ./cmd/vmafx-tune
# or with version injection:
go build -ldflags "-X main.version=$(cat VERSION)" -o vmafx-tune-go ./cmd/vmafx-tune
```

## Ported subcommands (Stage 1)

### `compare` — Rate-quality sweep

Runs a VMAF-target bisect for each `(codec, target)` pair and emits a ranked
report.

```text
vmafx-tune-go compare [flags]
```

**Required flags:**

| Flag | Description |
|------|-------------|
| `--reference`, `-r` | Path to the reference video file |

**Optional flags:**

| Flag | Default | Description |
|------|---------|-------------|
| `--codecs`, `-c` | `libx264,libx265` | Comma-separated encoder names. Stage-1 supports software encoders only (libx264, libx265). |
| `--targets`, `-t` | `85` | Comma-separated VMAF target(s). Multiple targets produce a schema-v2 sweep. |
| `--output`, `-o` | stdout | Output file path. |
| `--format` | `json` | Output format: `json` or `markdown`. |
| `--ffmpeg` | `ffmpeg` | Path to the ffmpeg binary. |
| `--vmaf` | `vmaf` | Path to the vmaf binary for scoring. |
| `--work-dir` | OS temp dir | Directory for temporary encode outputs. |
| `--crf-lo` | `0` | Lower bound of CRF search window (best quality). |
| `--crf-hi` | `0` | Upper bound (0 = encoder default: 51 for x264/x265). |
| `--max-iter` | `12` | Maximum bisect iterations per (codec, target) pair. |

**Example — single target, JSON output:**

```bash
vmafx-tune-go compare \
  --reference src.mp4 \
  --codecs libx264,libx265 \
  --targets 85 \
  --output results.json
```

**Example — multi-target sweep, Markdown:**

```bash
vmafx-tune-go compare \
  --reference src.mp4 \
  --codecs libx264 \
  --targets 80,85,90,95 \
  --format markdown
```

### Output schema

Single-target output (`--targets` has one value) uses schema-v1, identical to
the Python `vmaf-tune compare` JSON output:

```json
{
  "src": "src.mp4",
  "target_vmaf": 85.0,
  "tool_version": "dev",
  "wall_time_ms": 4200,
  "rows": [
    {
      "codec": "libx264",
      "best_crf": 23,
      "bitrate_kbps": 1234.5,
      "encode_time_ms": 2100,
      "vmaf_score": 85.34,
      "target_vmaf": 85.0,
      "ok": true,
      "error": "",
      "bisect_samples": [...]
    }
  ]
}
```

Multi-target output uses schema-v2 (adds `schema_version: 2` and
`target_vmafs: [...]`), also compatible with the Python `report.py` renderer.

Non-finite float values (`NaN`, `Inf`) are serialized as `null` — the output is
RFC 8259 strict JSON, parseable by Go, Rust, and `jq --strict`.

## Ported subcommands (Stage 4)

### `report` — Render Markdown or HTML from prior runs

Reads one or more JSON files produced by `compare` or `ladder` and renders a
human-readable report.

```text
vmafx-tune-go report <input.json> [input2.json ...] [flags]
```

The subcommand auto-detects whether each input is a `compare` or `ladder` JSON
file and renders a unified report.

**Optional flags:**

| Flag | Default | Description |
|------|---------|-------------|
| `--output`, `-o` | stdout | Output file path. |
| `--format` | `markdown` | Output format: `markdown` or `html`. |

**Example — Markdown to stdout:**

```bash
vmafx-tune-go report results.json
```

**Example — merge compare + ladder results into one HTML report:**

```bash
vmafx-tune-go report compare.json ladder.json \
  --format html \
  --output report.html
```

The HTML output is self-contained (inlined CSS, no external dependencies) and
renders correctly without a network connection.

### `fast` — Proxy + TPE + GPU-verify recommend

Recommends a CRF for a VMAF target without running the full Phase A grid.
Instead of a sweep, a TPE (Tree-structured Parzen Estimator) search walks the
integer CRF axis: each trial encodes a short probe slice, extracts the
canonical-6 libvmaf features, and predicts VMAF with the `fr_regressor_v2`
proxy. A single real encode + libvmaf score at the chosen CRF then verifies the
recommendation — **the proxy alone never wins** (ADR-0304).

```text
vmafx-tune-go fast --target-vmaf <N> [--smoke | --src <file> --width W --height H] [flags]
```

> **Port status.** `--smoke` runs end to end today. Production mode runs the
> backend selection, the probe encodes, the canonical-6 extraction and the
> verify pass, but stops at the proxy-inference step: `fr_regressor_v2` is a
> two-named-input ONNX graph and the Go inference seam drives a single flat
> input vector only. See [Production-mode
> blocker](#production-mode-blocker-onnx-named-inputs) below and use
> `vmaf-tune fast` for a production run in the meantime.

**Required flags:**

| Flag | Description |
|------|-------------|
| `--target-vmaf` | Quality target on the standard VMAF `[0, 100]` scale |

**Optional flags** (names and defaults match `vmaf-tune fast`):

| Flag | Default | Description |
|------|---------|-------------|
| `--src` | — | Source video (raw YUV or any ffmpeg-readable container). Required unless `--smoke`. |
| `--width` / `--height` | `0` | Raw-YUV reference geometry. Required in production mode. |
| `--pix-fmt` | `yuv420p` | ffmpeg pixel format. |
| `--framerate` | `24` | Reference framerate. |
| `--encoder` | `libx264` | Codec adapter. Must be in the proxy model's encoder vocabulary in production mode. |
| `--preset` | `medium` | Encoder preset for the probe + verify encodes. |
| `--crf-min` / `--crf-max` | `10` / `51` | Inclusive CRF search range. |
| `--n-trials` | `30` prod / `50` smoke | TPE trial budget. |
| `--time-budget-s` | `300` | Soft wall-clock cap on the TPE loop. In-flight trials are allowed to finish. |
| `--proxy-tolerance` | `1.5` | Max absolute proxy/verify VMAF gap before the result is flagged out-of-distribution. |
| `--sample-chunk-seconds` | `5.0` | Probe-encode slice length per trial. Shorter = faster trials, longer = more stable features. |
| `--smoke` | `false` | Deterministic synthetic CRF→VMAF curve. No ffmpeg, no ONNX, no GPU verify. |
| `--score-backend` | `auto` | libvmaf backend for the verify pass: `auto`, `cpu`, `cuda`, `sycl`, `hip`. `auto` walks cuda → sycl → hip → cpu; an explicit value is honoured strictly and errors rather than downgrading. |
| `--ffmpeg-bin` | `ffmpeg` | Path to the ffmpeg binary. |
| `--vmaf-bin` | `vmaf` | Path to the libvmaf CLI binary. |
| `--vmaf-model` | `vmaf_v0.6.1` | vmaf model version string. |
| `--encode-dir` | `.workingdir2/fast` | Scratch dir for probe + verify encodes. |
| `--output`, `-o` | stdout | JSON destination for the recommendation payload. |

**Exit codes** (identical to `vmaf-tune fast`):

| Code | Meaning |
|------|---------|
| `0` | Recommendation emitted; proxy and verify agree within `--proxy-tolerance`. |
| `2` | Usage or environment error (bad CRF range, missing `--src`, unavailable backend, proxy unavailable). |
| `3` | Recommendation emitted, but the proxy/verify gap exceeds tolerance. Fall back to the slow Phase A grid (ADR-0276). The payload is still written. |

**Example — smoke run (works on any host):**

```bash
vmafx-tune-go fast --smoke --target-vmaf 90
```

```json
{
  "encoder": "libx264",
  "n_trials": 50,
  "notes": "smoke mode — synthetic predictor; no ffmpeg / ONNX / GPU. See ADR-0276 + ADR-0304 + Research-0076 for the production path.",
  "predicted_kbps": 3486.832605990455,
  "predicted_vmaf": 90.3551546220283,
  "proxy_verify_gap": null,
  "recommended_crf": 20,
  "smoke": true,
  "target_vmaf": 90.0,
  "verify_vmaf": null
}
```

The payload is byte-compatible with the Python `vmaf-tune fast` output: keys are
sorted, floats are rendered with CPython's `repr` formatting (so an integral
`target_vmaf` prints as `90.0`, not `90`), and `verify_vmaf` /
`proxy_verify_gap` are `null` in smoke mode. Production mode adds one key,
`score_backend`, naming the selected libvmaf backend.

**Example — production run (once the ONNX blocker is lifted):**

```bash
vmafx-tune-go fast \
  --src ref_1920x1080.yuv --width 1920 --height 1080 --framerate 24 \
  --target-vmaf 93 --encoder libx264 --preset medium \
  --score-backend cuda \
  --output fast.json
```

#### Production-mode blocker: ONNX named inputs

The shipped proxy `model/tiny/fr_regressor_v2.onnx` declares **two named input
ports** — `features` (shape `[N, 6]`) and `codec` (shape `[N, 14]`) — as
recorded in its sidecar's `"input_names"`. The only ONNX inference seam in the
Go tree, `pkg/ai.Registry.Infer`, serialises a single flat `[]float64` to the
`vmafx-ort-runner` subprocess and has no wire format for a second port;
`Registry.InferDirect`, the CGO path, is an explicit Stage-2 stub.

Flattening the two ports into one 20-D vector is **not** a workaround —
`vmaftune/proxy.py` documents that exact mistake: the graph's first dense layer
reads the 6-D `features` port only, so the 14 codec dimensions are silently
interpreted as batch padding and `codec` receives nothing. Rather than return a
quietly-wrong score, `pkg/fast` fails with `ErrProxyPortsUnsupported` and a
diagnostic naming both ports.

Any one of these unblocks it:

1. A `vmafx-ort-runner` protocol that accepts named input tensors, plus a
   matching `pkg/ai.Registry.InferNamed`.
2. Promoting `pkg/ai.Registry.InferDirect` onto a CGO ONNX Runtime binding
   (e.g. `github.com/yalue/onnxruntime_go`), which `pkg/ai` defers to Stage 2
   precisely because it couples the build to `libonnxruntime`.
3. A single-port re-export of `fr_regressor_v2` that concatenates the two
   inputs *inside* the graph, shipped alongside the current model.

#### Divergences from the Python `fast` implementation

The Go port fixes four defects found in `vmaf-tune fast` while reading it. The
CLI surface and the JSON schema are unchanged; only the numbers the proxy would
see differ.

| # | Python behaviour | Go behaviour |
|---|------------------|--------------|
| 1 | `cli._build_fast_sample_extractor` hands the probe `.mp4` straight to the libvmaf CLI, which reads raw YUV only — every probe score fails and the feature vector degrades to six zeros. | Container-shaped encodes are decoded to raw YUV first (the `score.maybe_decode_distorted` step the Python probe leg skips), on both the probe and verify legs. |
| 2 | `cli._parse_canonical6_means` looks up bare `adm2` / `vif_scale0` keys in `pooled_metrics`; modern libvmaf emits `integer_adm2` / `integer_vif_scale0`. `score.py` knows this and carries a mapping, but the fast path does not use it. | The `integer_`-prefixed key is tried first, then the bare key, then a per-frame average of either. |
| 3 | `proxy.py`'s hardcoded `ENCODER_VOCAB_V2` disagrees with the trainer and the shipped sidecar from index 3 on (`libaom-av1` vs `libvvenc`), so the codec one-hot lands in the wrong slot for every codec past `libsvtav1`, and the model's own `unknown` catch-all is unreachable. | The vocabulary is read from the model sidecar's `encoder_vocab`, so it cannot drift from the installed checkpoint. Out-of-vocabulary codecs map to `unknown` when the model has that slot, and are a hard error otherwise. |
| 4 | The sidecar ships `feature_mean` / `feature_std`, and `run_proxy` documents that the caller must apply them — but no caller on the fast path does, so raw libvmaf means reach a model trained on standardised features. | The sidecar's StandardScaler is applied before inference. |

One behaviour is *weaker* in Go than in Python:

- **TPE reproducibility.** Optuna's `TPESampler(seed=0)` makes a run
  bit-reproducible. The Go port uses `github.com/c-bata/goptuna`, whose TPE
  sampler honours its seed only partially: `tpe.SamplerOptionSeed` seeds the
  sampler's own RNG and its startup random sampler, but
  `goptuna/internal/random.ArgMaxMultinomial` draws from the *process-global*
  `math/rand` source, which Go seeds randomly at startup and which
  `rand.Seed` can no longer override. Repeat runs therefore explore slightly
  different trial sequences and may return a neighbouring CRF when two
  candidates score within about a VMAF point of each other. Measured on the
  ADR-0276 smoke curve: at the shipped budgets the recommendation stays within
  ±1 of the brute-force optimum in ~99–100 % of runs, and at 150 trials it hit
  the exact optimum in 150 of 150 runs for every target tested. Closing the gap
  needs an upstream goptuna change threading the sampler RNG into
  `internal/random`.

## Not yet ported (Stage 5+)

The following subcommands are stubs in `vmafx-tune-go`. They log a redirect
notice (a `WARN`-level structured log line) and exit 1 when invoked. Use the
Python `vmaf-tune` binary for these:

| Subcommand | Python equivalent |
|------------|-------------------|
| `tune-per-shot` | `vmaf-tune tune-per-shot` |
| `corpus` | `vmaf-tune corpus` |
| `benchmark` | `vmaf-tune benchmark` |
| `auto` | `vmaf-tune auto` |
| `sidecar` | `vmaf-tune sidecar` |
| `encode-profile` | `vmaf-tune encode-profile` |

## Configuration and logging

`vmafx-tune-go` runs each subcommand inside the golusoris `clikit` (cobra + fx)
framework. The framework injects a structured `*slog.Logger` and a config tree
into every subcommand, so run diagnostics (sweep start/finish, ladder build
summary, report rendering) are emitted as structured log lines on `stderr`,
while subcommand output (JSON / Markdown / HTML) goes to `stdout` or the
`--output` file. This keeps machine-readable output separate from logs when you
pipe `stdout`.

Configuration is read from environment variables under the `VMAFX_` prefix.
golusoris maps each underscore in the variable name to a config-path delimiter
(`VMAFX_LOG_LEVEL` → `log.level`):

| Environment variable | Config key | Effect | Default |
|----------------------|------------|--------|---------|
| `VMAFX_LOG_LEVEL` | `log.level` | Minimum log level: `debug`, `info`, `warn`, `error` | `info` |
| `VMAFX_LOG_FORMAT` | `log.format` | Log handler: `auto` (tint on a TTY, JSON otherwise), `tint`, `json` | `auto` |

Examples:

```bash
# Quiet the per-run INFO diagnostics; keep warnings/errors.
VMAFX_LOG_LEVEL=warn vmafx-tune-go report results.json

# Force JSON logs for machine ingestion regardless of TTY.
VMAFX_LOG_FORMAT=json vmafx-tune-go compare --reference src.mp4 --targets 90
```

## Migration roadmap

| Stage | Scope | ADR | Status |
|-------|-------|-----|--------|
| Stage 1 | `compare` subcommand, libx264/libx265, single/multi-target bisect | ADR-0705 | Merged |
| Stage 2 | `ladder` subcommand, hardware encoders (NVENC, QSV, AMF), convex hull + knee selection | ADR-0730 | Merged |
| Stage 3 | Downscale plumbing | — | Merged |
| Stage 4 | `report` subcommand, Markdown + HTML rendering | ADR-0770 | Merged |
| golusoris | Migrate the CLI root + subcommands onto the golusoris `clikit` (cobra + fx) framework; `VMAFX_`-prefixed config + injected `slog` | ADR-1119 | Merged |
| Stage 6 | `fast` subcommand + `pkg/conformal` + `pkg/scorebackend`; smoke path complete, production path blocked on ONNX named inputs | ADR-0276 / ADR-0304 | **This PR** |
| Stage 5 | `tune-per-shot` subcommand, conformal CLI wiring | Planned | — |
| Stage 6b | `fast` production mode (needs a named-input ONNX seam — see [Production-mode blocker](#production-mode-blocker-onnx-named-inputs)) | Planned | — |
| Stage N | Feature parity; rename binary to `vmafx-tune` | Planned | — |

> **Correction.** An earlier revision of this table listed `pkg/conformal` as
> merged under Stage 3. It was not: no such package existed on `master`. The
> package landed with Stage 6 above, and the roadmap row has been corrected.

## Architecture

The CLI root and every subcommand are built with the golusoris `clikit`
(cobra + fx) framework (ADR-1119): `clikit.New` builds the root, `clikit.Command`
builds each subcommand, and a thin `withGolusoris` adapter boots a one-shot fx
graph per invocation so the command receives an injected `*slog.Logger` and
config, runs to completion, and propagates its error as the process exit code.

The Go binary uses an **adapter pattern** with these core packages:

- **`pkg/encoder/`** — `Encoder` interface + software and hardware encoder implementations.
  Each encoder shells out to `ffmpeg`; no `libavcodec` CGo dependency.
  `EncodeParams.InputArgs` carries ffmpeg *input-side* options so raw-YUV
  sources (`-f rawvideo -pix_fmt -s -r`) and sample clips (input-side
  `-ss` / `-t`, for fast-seek) work; `EncodeParams.OutputPath` pins a
  deterministic destination and `EncodeResult.OutputSizeBytes` reports the
  encode size for size-over-duration bitrate maths.
- **`pkg/bisect/`** — Stateless `Run(src, enc, scoreFunc, params)` function.
  The score function is injectable, enabling unit tests without a live `vmaf` binary.
- **`pkg/ladder/`** — `Build(src, encoder, Params)` function. Convex hull
  (`upperConvexHull`), knee selection (`selectRenditions`), min-bitrate-gap filter.
- **`pkg/report/`** — Stage-1: `EmitJSON` / `EmitMarkdown` renderers (single-run emit).
  Stage-4: `RenderMarkdownMulti` / `RenderHTMLMulti` (multi-file report rendering).
- **`pkg/fast/`** — the fast path: `Recommend` (the flow), `RunTPE` (the
  goptuna-backed search), `NewSamplePredictor` / `NewVerifier` (the probe and
  verify pipelines), and `ORTProxy` (the `fr_regressor_v2` seam).
- **`pkg/scorebackend/`** — libvmaf backend detection and strict selection,
  ported from the selection half of `vmaftune/score_backend.py`. `Detect`
  intersects what the local `vmaf --help` advertises with what
  `nvidia-smi` / `sycl-ls` / `rocminfo` report; `Select` honours `auto` via a
  fallback chain and never silently downgrades an explicit request.
- **`pkg/conformal/`** — distribution-free prediction intervals for the VMAF
  predictor (split conformal and CV+ / jackknife+), ported from
  `vmaftune/conformal.py`. The JSON sidecar is byte-compatible with the Python
  writer, so a calibration produced by either implementation loads in the
  other. Not yet wired into a CLI flag — that is Stage 5.

See [ADR-0705](../adr/0705-vmafx-tune-go-stage1.md) for the migration rationale,
[ADR-0730](../adr/0730-vmafx-tune-go-stage2.md) for Stage-2,
[ADR-0770](../adr/0770-vmafx-tune-go-stage4-report.md) for Stage-4, and
[ADR-0702](../adr/0702-vmafx-phase4-language-modernization.md) for the Phase 4 umbrella.
