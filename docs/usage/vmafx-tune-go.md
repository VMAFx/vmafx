<!-- markdownlint-disable MD013 MD060 -->
# vmafx-tune-go — Go port of vmaf-tune (Stage 4)

`vmafx-tune-go` is the Go port of the `vmaf-tune` rate-quality tuning CLI
(Stages 1–4). It ships as a **separate binary** alongside the Python `vmaf-tune`
binary during the migration. The Python binary is unchanged and should be used
for all subcommands that are not yet ported.

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

## Ported subcommands (ML-driven group)

These four subcommands cover the predictor-, saliency- and search-driven half
of the CLI. Their JSON payloads are byte-identical to the Python originals —
key order, `", "` / `": "` separators, `NaN` / `Infinity` tokens and float
rendering all match `json.dumps` — so a downstream consumer cannot tell which
binary produced a file.

### `recommend` — pick the CRF meeting a target

Two modes. `--from-corpus` picks from an existing corpus JSONL with no new
encodes; without it, a coarse-to-fine CRF search runs the encodes first,
writes every visited point to `--output`, and picks from those rows.

```text
vmafx-tune-go recommend [flags]
```

**Predicates (mutually exclusive):**

| Flag | Description |
|------|-------------|
| `--target-vmaf` | Smallest CRF whose VMAF meets the target. A smaller CRF is higher quality, so this is the best quality that clears the gate. Falls back to the closest miss, tagged `(UNMET)`, when nothing clears it. |
| `--target-bitrate` | Row whose `bitrate_kbps` is closest to the target; ties go to the lower CRF. `--from-corpus` only. |

**Source and encode flags** (required unless `--from-corpus` is used):

| Flag | Default | Description |
|------|---------|-------------|
| `--source` | — | Reference video; repeatable to sweep several sources. |
| `--width` / `--height` | — | Raw-YUV reference geometry. |
| `--preset` | — | Encoder preset; repeatable to sweep several presets. |
| `--encoder` | `libx264` | Codec adapter. |
| `--pix-fmt` | `yuv420p` | ffmpeg pix_fmt. |
| `--framerate` | `24` | Reference framerate. |
| `--duration` | `0` | Clip duration in seconds; bounds the encode and derives achieved kbps. |
| `--output` | `corpus.jsonl` | JSONL destination for the visited points. |
| `--encode-dir` | `.workingdir2/encodes` | Scratch directory for the probe encodes. |
| `--keep-encodes` | off | Keep the encoded artefacts instead of deleting them after scoring. |
| `--no-source-hash` | off | Skip the source SHA-256 (faster on very large sources). |
| `--vmaf-model` | `vmaf_v0.6.1` | libvmaf model version, or a `path=...` string. |
| `--score-backend` | `auto` | libvmaf backend: `auto`, `cpu`, `cuda`, `sycl`, `hip`. |
| `--ffmpeg-bin` / `--vmaf-bin` | `ffmpeg` / `vmaf` | Binary paths. |

**Search flags:**

| Flag | Default | Description |
|------|---------|-------------|
| `--coarse-step` | `10` | CRF step for the coarse pass (10 → 10, 20, 30, 40, 50). |
| `--fine-radius` | `5` | Radius around the best-coarse CRF for the fine pass. |
| `--fine-step` | `1` | CRF step for the fine pass. |

With the defaults that is 5 coarse plus up to 10 fine encodes, against 42 for a
full 10–50 sweep (ADR-0296). The CRF window is fixed at 10–50, matching the
Python defaults, so a Go and a Python run over the same source visit the same
cells and their corpora stay comparable.

**Uncertainty flags (ADR-0279):**

| Flag | Default | Description |
|------|---------|-------------|
| `--with-uncertainty` | off | Consume the conformal prediction intervals carried in each row's `vmaf_interval` block. |
| `--uncertainty-sidecar` | — | Calibration sidecar JSON. Without one the documented Research-0067 floor (tight 2.0, wide 5.0 VMAF) applies. |

A tight interval whose lower bound already clears the target short-circuits the
search at that row; a wide interval refuses to short-circuit and tags the result
`(UNCERTAIN)`. This changes which encodes get **probed**, never which get
**shipped** — the production-flip gate stays in the predictor's validation
harness.

**Examples:**

```bash
# Pick from an existing corpus, machine-readable.
vmafx-tune-go recommend --from-corpus corpus.jsonl --target-vmaf 93 --json

# Run the search, then pick.
vmafx-tune-go recommend \
  --source src.yuv --width 1920 --height 1080 --duration 10 \
  --preset medium --target-vmaf 93 --output corpus.jsonl
```

### `predict` — predict per-shot VMAF, then verify

Predicts VMAF per shot from cheap signals, then verifies the prediction against
real libvmaf on K stratified shots and emits a verdict.

```text
vmafx-tune-go predict --source <video> [flags]
```

| Flag | Default | Description |
|------|---------|-------------|
| `--source` | — | Reference video, any FFmpeg-readable container (required). |
| `--codec` | `libx264` | Codec adapter. |
| `--target-vmaf` | `93` | Target pooled-mean VMAF. |
| `--validate-k` | `8` | Shots to verify against real libvmaf. |
| `--residual-threshold` | `1.5` | Max `abs(predicted - measured)` before the verdict leaves `gospel`. |
| `--model` | — | `predictor_<codec>.onnx`; without it the per-codec analytical curve runs. |
| `--use-saliency` | off | Include saliency mean/variance in the feature vector. |
| `--saliency-model` | shipped model | `saliency_student_v1.onnx` path. |
| `--per-shot-bin` | `vmaf-perShot` | Shot detector binary. |
| `--bitdepth` | `8` | Source bit depth (8, 10 or 12), forwarded to the detector. |
| `--total-frames` | `0` | Frame count for the single-shot fallback. |
| `--report-out` | stdout | Validation report destination. |
| `--with-uncertainty` | off | Emit conformal intervals beside each predicted VMAF. |
| `--calibration-sidecar` | — | Split-conformal calibration JSON. |
| `--alpha` | sidecar's | Override the miscoverage level (0.05 = 95 % coverage). |

**Verdicts and exit codes:**

| Verdict | Meaning | Exit |
|---------|---------|------|
| `gospel` | Every residual within the threshold; trust the predictor on the remaining shots. | 0 |
| `recalibrate` | Residuals biased but tight; add the reported `bias_correction` and redo the picks. No retraining needed. | 0 |
| `fall_back` | Residuals too wide; degrade to the full encode-and-score loop. | 2 |

Shot detection degrades to a single shot spanning the clip when `vmaf-perShot`
is unavailable, with a `WARN` log line. Without `--calibration-sidecar`,
`--with-uncertainty` emits degenerate `low == high == point` intervals and flags
the report `"calibrated": false` — do not read a coverage guarantee into a
zero-width interval.

```bash
vmafx-tune-go predict --source movie.mkv --codec libx264 \
  --target-vmaf 93 --validate-k 8 --report-out predict.json
```

### `recommend-saliency` — saliency-aware ROI encode

Scores the fork's `saliency_student_v1` model over sampled frames, reduces the
result to a per-block QP-offset map, and hands it to the encoder through its
native ROI channel.

```text
vmafx-tune-go recommend-saliency --src <yuv> --width W --height H \
  --duration-frames N --output <path> [flags]
```

| Flag | Default | Description |
|------|---------|-------------|
| `--saliency-aware` | off | Enable the biasing; without it this is a plain encode. |
| `--saliency-offset` | `-4` | QP delta at peak saliency, clamped to ±12. Negative spends more bits on salient regions. |
| `--saliency-aggregator` | `mean` | Temporal reducer: `mean`, `ema`, `max` or `motion-weighted`. |
| `--saliency-ema-alpha` | `0.6` | Current-frame weight for `ema`. |
| `--saliency-model` | shipped model | ONNX path. |
| `--saliency-fallback-plain` | off | Accept a plain encode on an encoder with no ROI dispatch instead of exiting 2 (ADR-0546). |
| `--encoder` | `libx264` | Codec adapter. |
| `--crf` | adapter default | Explicit CRF. |
| `--preset` | `medium` | Encoder preset. |

**ROI channel per encoder:**

| Encoder | Channel | Granularity |
|---------|---------|-------------|
| `libx264` | `-x264-params qpfile=…` | 16×16 macroblocks |
| `libaom-av1` | `-qpfile …` (patched FFmpeg bridge) | 16×16 macroblocks |
| `libx265` | `-x265-params zones=…` | per-clip spatial mean |
| `libsvtav1` | `-svtav1-params qp-file=…` | 64×64 super-blocks |
| `libvvenc` | `-vvenc-params ROIFile=…` | 64×64 CTUs |

Any other encoder exits 2 unless `--saliency-fallback-plain` is set or
`VMAFTUNE_SALIENCY_FALLBACK_OK=1` is exported.

An `--output` ending in `.json` is treated as a report destination: the encode
goes to a sibling `<stem>_encoded.mp4` and the path is echoed on stdout.

> **Inference availability.** The Go port ships the entire numeric pipeline —
> YUV to ImageNet tensor, all four temporal aggregators, the QP mapping, the
> per-block reduce and every sidecar format — but has no in-process ONNX
> Runtime. With `--saliency-aware` it therefore logs a warning and falls back
> to a plain encode, the same degradation the Python takes when `onnxruntime`
> is not installed. See [Known gaps](#known-gaps) below.

### `prefilter` — joint deband + CRF autotune

Optimises the ten frozen Pelorus deband knobs (the ADR-0110 control-plane
contract) and the CRF axis together in one TPE study, with VMAF as the oracle.

```text
vmafx-tune-go prefilter --target-vmaf T [flags]
```

| Flag | Default | Description |
|------|---------|-------------|
| `--target-vmaf` | — | Quality target on the VMAF [0, 100] scale (required). |
| `--smoke` | off | Use the synthetic surface; no ffmpeg, Vulkan or GPU. |
| `--src` | — | Source video; required for the live loop. |
| `--width` / `--height` | — | Raw-YUV geometry; required for the live loop. |
| `--sweep-knob` | all ten | Restrict the search to this knob; repeatable. |
| `--crf-min` / `--crf-max` | `18` / `40` | Joint search range. |
| `--n-trials` | `60` live, `40` smoke | TPE trial budget. |
| `--time-budget-s` | `600` | Soft wall-clock cap. |
| `--seed` | `0` | Sampler seed; the same seed reproduces the same recommendation. |
| `--encoder` | `libx264` | Codec performing the post-deband encode. |
| `--filter` | `pelorus_deband` | Filter adapter to autotune. |
| `--encode-dir` | `.workingdir2/prefilter` | Probe scratch directory. |
| `--output` | stdout | JSON destination. |

The objective is `|achieved - target| + λ·kbps`, so the search converges on the
lowest-bitrate combination that hits the target. The bitrate weight is small
enough that it only breaks ties between equally-good quality points.

vmafx never runs the deband filter itself — it only emits the `-vf` string — so
the live loop requires `pelorus_deband_vulkan` in the ffmpeg build and refuses
to start (exit 2) with an actionable message when it is absent. `--smoke`
exercises the whole search without it.

The ten swept knobs are `range`, `thry`, `thrc`, `grainy`, `grainc`,
`softness`, `detail`, `dither`, `dynamic` and `protect`. The deliberately
out-of-contract options (`sample`, `blur`, `planes`, `meta`) are pipeline
switches set once per run and are rejected if passed to `--sweep-knob`.

```bash
# CI-friendly: no ffmpeg, no Vulkan, no GPU.
vmafx-tune-go prefilter --smoke --target-vmaf 93 --output rec.json

# Live loop.
vmafx-tune-go prefilter \
  --src ref.yuv --width 1920 --height 1080 --duration 10 \
  --target-vmaf 93 --encoder libx264 --output rec.json
```

Unlike the Python subcommand, this needs no optional `[fast]` extra: the TPE
sampler is implemented natively (see [Known gaps](#known-gaps)).

## Known gaps

Two behaviours in this group are not at full parity. Both are documented here
rather than hidden behind a silent degradation.

### Saliency ONNX inference

`recommend-saliency --saliency-aware` falls back to a plain encode. Everything
around the model is ported and tested against the Python — only the single
forward pass is missing, because:

- Go has no in-process ONNX Runtime in this module. The fork's bridge
  (`pkg/ai`, ADR-0713) shells out to `vmafx-ort-runner` and passes the input
  tensor as a JSON array in **argv**. That works for the per-shot predictor's
  14 floats; it cannot carry saliency's 3×H×W input, which is 6.2 million
  floats (about 75 MB of JSON) for a 1080p frame.
- `vmafx-ort-runner` is not built from this repository; nothing under `cmd/`
  produces it.

Either of two changes unblocks it: a cgo ONNX Runtime binding (an ADR-level
decision, since the binary currently builds without cgo), or a runner protocol
that streams tensors over stdin plus a `cmd/vmafx-ort-runner` target here.

The per-shot **predictor** ONNX (`predict --model`) does route through
`pkg/ai`, because its 14-float input fits argv comfortably; it degrades to the
analytical curve when the runner is absent.

### TPE sampler trajectory

`prefilter` implements TPE natively (Bergstra et al. 2011 §4, the construction
Optuna follows) rather than depending on a Go Optuna port that would pull gorm
plus the MySQL, Postgres and cgo-SQLite drivers into a one-shot CLI. The search
space, the objective, the emitted JSON and per-seed reproducibility are
identical to the Python; the trial-by-trial trajectory for a given seed is not,
and cannot be, because the two use different RNG streams.

## Not yet ported (Stage 5+)

The following subcommands are stubs in `vmafx-tune-go`. They log a redirect
notice (a `WARN`-level structured log line) and exit 1 when invoked. Use the
Python `vmaf-tune` binary for these:

| Subcommand | Python equivalent |
|------------|-------------------|
| `tune-per-shot` | `vmaf-tune tune-per-shot` |
| `fast` | `vmaf-tune fast` |
| `corpus` | `vmaf-tune corpus` |
| `benchmark` | `vmaf-tune benchmark` |
| `auto` | `vmaf-tune auto` |
| `sidecar` | `vmaf-tune sidecar` |
| `encode-profile` | `vmaf-tune encode-profile` |

`recommend`'s encode-driven path writes the same schema-v3 corpus JSONL the
`corpus` subcommand does, and every key is present. Five corpus features are
not carried by this group's port and their fields hold the same zero / empty
values the Python emits when the feature is unavailable, so a reader filters on
them exactly as it already does: the content-addressed encode cache
(ADR-0298), HDR detection (ADR-0295), TransNet-V2 shot metadata
(`shot_count` stays 0), sample-clip windowing (`clip_mode` stays `full`), and
the encoder-internal pass-1 stats (the ten `enc_internal_*` columns stay 0.0,
which is what the Python aggregator returns for an empty frame list). Those
belong to the `corpus` port.

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
| Stage 3 | `pkg/conformal`, downscale plumbing | — | Merged |
| Stage 4 | `report` subcommand, Markdown + HTML rendering | ADR-0770 | Merged |
| golusoris | Migrate the CLI root + subcommands onto the golusoris `clikit` (cobra + fx) framework; `VMAFX_`-prefixed config + injected `slog` | ADR-1119 | Merged |
| ML-driven | `recommend`, `predict`, `recommend-saliency`, `prefilter`; codec-adapter registry, encode/score drivers, predictor, saliency pipeline, native TPE | — | **This PR** |
| Stage 5 | `tune-per-shot` subcommand, conformal CLI wiring | Planned | — |
| Stage 6 | `fast` subcommand (requires ONNX Go binding) | Planned | — |
| Stage N | Feature parity; rename binary to `vmafx-tune` | Planned | — |

## Architecture

The CLI root and every subcommand are built with the golusoris `clikit`
(cobra + fx) framework (ADR-1119): `clikit.New` builds the root, `clikit.Command`
builds each subcommand, and a thin `withGolusoris` adapter boots a one-shot fx
graph per invocation so the command receives an injected `*slog.Logger` and
config, runs to completion, and propagates its error as the process exit code.

The Go binary uses an **adapter pattern** with four core packages:

- **`pkg/encoder/`** — `Encoder` interface + software and hardware encoder implementations.
  Each encoder shells out to `ffmpeg`; no `libavcodec` CGo dependency.
- **`pkg/bisect/`** — Stateless `Run(src, enc, scoreFunc, params)` function.
  The score function is injectable, enabling unit tests without a live `vmaf` binary.
- **`pkg/ladder/`** — `Build(src, encoder, Params)` function. Convex hull
  (`upperConvexHull`), knee selection (`selectRenditions`), min-bitrate-gap filter.
- **`pkg/report/`** — Stage-1: `EmitJSON` / `EmitMarkdown` renderers (single-run emit).
  Stage-4: `RenderMarkdownMulti` / `RenderHTMLMulti` (multi-file report rendering).

The ML-driven group adds:

- **`pkg/codecadapter/`** — the nineteen-codec registry (quality windows, preset
  mapping onto each encoder's native axis, probe argv, two-pass argv).
- **`pkg/ffencode/`** — the encode driver the tuning subcommands share: raw-YUV
  geometry, named presets, injected extra params, sample-clip windowing.
  Distinct from `pkg/encoder`, which models the narrower Stage-1 bisect
  abstraction.
- **`pkg/scorecli/`** — the libvmaf CLI driver with explicit geometry, the
  backend selector and the canonical-6 pooled aggregates.
- **`pkg/predictor/`** — `ShotFeatures`, the per-codec analytical curve, the
  binary-search CRF inversion, feature extraction and the validation harness.
- **`pkg/pershot/`** — shot detection via `vmaf-perShot` with the single-shot
  fallback and the uniform long-shot splitter.
- **`pkg/saliency/`** — the full saliency ROI pipeline: YUV to ImageNet tensor,
  four temporal aggregators, QP mapping, per-block reduce, five sidecar formats.
- **`pkg/prefilter/`** — the frozen Pelorus knob contract plus a native TPE
  sampler.
- **`pkg/recommend/`**, **`pkg/conformal/`**, **`pkg/uncertainty/`**,
  **`pkg/corpusrow/`** — the predicate pickers, split-conformal intervals,
  confidence bands and the schema-v3 corpus row.
- **`internal/pyjson/`** — renders payloads the way Python's `json.dumps` does,
  so the emitted JSON is byte-identical to the Python binary's.

See [ADR-0705](../adr/0705-vmafx-tune-go-stage1.md) for the migration rationale,
[ADR-0730](../adr/0730-vmafx-tune-go-stage2.md) for Stage-2,
[ADR-0770](../adr/0770-vmafx-tune-go-stage4-report.md) for Stage-4, and
[ADR-0702](../adr/0702-vmafx-phase4-language-modernization.md) for the Phase 4 umbrella.
