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

## Ported subcommands (Stage 5)

### `tune-per-shot` — Per-shot CRF tuning

Cuts the source into shots, bisects a CRF against the VMAF target inside each
shot, and emits an FFmpeg **encoding plan**: one command per segment plus the
concat-demuxer command that stitches them into the final file.

The plan is emitted, **not executed**. Segment encodes are independent, so you
can run them sequentially or in parallel, then run the concat command.

```text
vmafx-tune-go tune-per-shot --src <video> [flags]
```

Pipeline:

1. **Shot detection** — shells out to the fork's `vmaf-perShot` binary
   ([vmaf-perShot.md](vmaf-perShot.md)), which wraps TransNet V2. A missing or
   failing binary degrades to one shot spanning the clip.
2. **Uniform-window splitter** — any shot longer than `--max-shot-duration` is
   sliced into equal sub-shots, so an under-cutting detector (fades, short
   clips) still yields a usable timeline.
3. **Per-shot bisect** — each shot is extracted to raw YUV and run through the
   CRF bisect against `--target-vmaf`.
4. **Plan emission** — the recommendations become segment + concat commands.

**Required flags:**

| Flag | Description |
|------|-------------|
| `--src` | Reference video: raw YUV, or any FFmpeg-readable container |

**Source geometry:**

| Flag | Default | Description |
|------|---------|-------------|
| `--width` | auto-probed | Source width. **Required** for raw YUV (`.yuv` / `.raw`); auto-probed via `ffprobe` for containers. |
| `--height` | auto-probed | Source height. Same rule as `--width`. |
| `--pix-fmt` | `yuv420p` | Source pixel format. |
| `--framerate` | auto-probed | Source framerate. Falls back to `24.0` when the probe yields nothing. |
| `--bitdepth` | `8` | Source YUV bit depth: `8`, `10` or `12`. |
| `--total-frames` | `0` | Frame count for the single-shot fallback when `vmaf-perShot` is unavailable. |

**Shot detection:**

| Flag | Default | Description |
|------|---------|-------------|
| `--per-shot-bin` | `vmaf-perShot` | Path to the shot-detector binary. |
| `--scene-threshold` | detector default (12.0) | Override the detector's mean-absolute-luma-delta cut threshold. Lower yields more shots. |
| `--max-shot-duration` | `2.0` | Uniform-window splitter, in seconds. `0` disables it. |

**Tuning:**

| Flag | Default | Description |
|------|---------|-------------|
| `--target-vmaf` | `92` | Target pooled-mean VMAF per shot. |
| `--encoder` | `libx264` | Codec adapter (see the codec table below). |
| `--preset` | codec default (`medium`) | Preset for the bisect encodes. |
| `--crf-min` / `--crf-max` | codec absolute window | Inclusive bisect search bounds. Pass both or neither. |
| `--max-iterations` | `8` | Maximum encode+score rounds per shot. |
| `--vmaf-model` | `vmaf_v0.6.1` | Model passed to the `vmaf` binary. |
| `--neg` | off | Route the model to its NEG variant (`vmaf_v0.6.1neg`). See [vmaf-neg.md](../metrics/vmaf-neg.md). |
| `--score-backend` | `auto` | libvmaf backend: `auto`, `cpu`, `cuda`, `sycl`, `hip`. An explicit backend that the host cannot provide fails fast rather than silently downgrading. |
| `--vmaf-bin` / `--ffmpeg-bin` | `vmaf` / `ffmpeg` | Binary paths. |
| `--workdir` | `$VMAFTUNE_WORKDIR` or OS temp | Scratch space for encode / decode artefacts. Raw YUV decodes are large — point this at a volume with room. |
| `--max-concurrent-decodes` | `1` | Concurrent reference-YUV decodes. `1` is safest on space-constrained volumes. |

**Output:**

| Flag | Default | Description |
|------|---------|-------------|
| `--plan-out` | stdout | Destination for the JSON plan. |
| `--output` | `per_shot_encode.mp4` | Final concatenated encode path **named inside the plan**. |
| `--segment-dir` | `<output dir>/segments` | Directory the segment commands write into. |
| `--script-out` | — | Also write the plan as a copy-paste shell script. |

**Example:**

```bash
vmafx-tune-go tune-per-shot \
  --src src.mp4 \
  --target-vmaf 92 \
  --encoder libx264 \
  --plan-out plan.json \
  --segment-dir ./segments \
  --output final.mp4
```

#### Plan JSON schema

Byte-compatible with `vmaf-tune tune-per-shot`: keys are sorted, floats keep
Python's `repr()` form, and a shot whose predicate never measured a bitrate
carries `null` rather than `NaN`.

```json
{
  "concat_command": ["ffmpeg", "-y", "-hide_banner", "-f", "concat", "..."],
  "encoder": "libx264",
  "framerate": 24.0,
  "predicate": "bisect",
  "segment_commands": [["ffmpeg", "-y", "-hide_banner", "-ss", "0.000000", "..."]],
  "shots": [
    {
      "bitrate_kbps": 1234.57,
      "crf": 22,
      "end_frame": 48,
      "predicted_vmaf": 92.5,
      "start_frame": 0
    }
  ],
  "target_vmaf": 92.0
}
```

`start_frame` is inclusive and `end_frame` exclusive (half-open). The
`vmaf-perShot` sidecar uses an inclusive end frame; the tuner normalises it.

The concat listing (`concat.txt`) is written next to `--plan-out` when
`--segment-dir` is not given, matching the Python. Pass `--segment-dir`
explicitly to pin the listing and the segment commands to the same directory;
the command logs a `WARN` when the two diverge.

#### Supported codecs

`tune-per-shot` accepts the ten codecs the Go encoder registry can construct.
Each emits its own quality knob in the plan:

| Codec | Plan argv shape |
|-------|-----------------|
| `libx264`, `libx265` | `-c:v NAME -preset medium -crf N` |
| `libsvtav1` | `-c:v libsvtav1 -preset 7 -crf N` (integer preset) |
| `libaom-av1` | `-c:v libaom-av1 -cpu-used 4 -crf N` |
| `h264_nvenc`, `hevc_nvenc` | `-c:v NAME -preset p4 -cq N` |
| `h264_qsv`, `hevc_qsv` | `-c:v NAME -preset medium -global_quality N` |
| `h264_amf`, `hevc_amf` | `-c:v NAME -quality balanced -rc cqp -qp_i N -qp_p N` |

The Python registry carries seven more adapters — `av1_nvenc`, `av1_qsv`,
`av1_amf`, the four VideoToolbox encoders, `libvvenc` and `libvpx-vp9`. They
have no Go encoder implementation yet, so `--encoder` rejects them by name and
points at the Python binary.

#### Flags with no Go implementation

Both fail fast with an actionable message rather than being accepted and
silently ignored:

| Flag | Why | Use instead |
|------|-----|-------------|
| `--predicate-module MODULE:CALLABLE` | Loads a Python callable at runtime; Go has no runtime import. The Go equivalent is the `pershot.PredicateFn` seam, available to library callers. | `vmaf-tune tune-per-shot --predicate-module ...` |
| `--fast-nr` | NR early-elimination runs the `nr_metric_v1` ONNX model through `onnxruntime`; the Go binary has no ONNX runtime binding. | `vmaf-tune tune-per-shot --fast-nr` |

## Not yet ported (Stage 6+)

The following subcommands are stubs in `vmafx-tune-go`. They log a redirect
notice (a `WARN`-level structured log line) and exit 1 when invoked. Use the
Python `vmaf-tune` binary for these:

| Subcommand | Python equivalent |
|------------|-------------------|
| `fast` | `vmaf-tune fast` |
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
| Stage 3 | `pkg/conformal`, downscale plumbing | — | Merged |
| Stage 4 | `report` subcommand, Markdown + HTML rendering | ADR-0770 | Merged |
| golusoris | Migrate the CLI root + subcommands onto the golusoris `clikit` (cobra + fx) framework; `VMAFX_`-prefixed config + injected `slog` | ADR-1119 | Merged |
| Stage 5 | `tune-per-shot` subcommand: `pkg/pershot`, `pkg/scorebackend`, codec-adapter table, raw-YUV scorer | ADR-0705 | **This PR** |
| Stage 5b | `conformal` CLI wiring | Planned | — |
| Stage 6 | `fast` subcommand + `tune-per-shot --fast-nr` (both require an ONNX Go binding) | Planned | — |
| Stage N | Feature parity; rename binary to `vmafx-tune` | Planned | — |

## Architecture

The CLI root and every subcommand are built with the golusoris `clikit`
(cobra + fx) framework (ADR-1119): `clikit.New` builds the root, `clikit.Command`
builds each subcommand, and a thin `withGolusoris` adapter boots a one-shot fx
graph per invocation so the command receives an injected `*slog.Logger` and
config, runs to completion, and propagates its error as the process exit code.

The Go binary uses an **adapter pattern** with these core packages:

- **`pkg/encoder/`** — `Encoder` interface + software and hardware encoder implementations.
  Each encoder shells out to `ffmpeg`; no `libavcodec` CGo dependency. Stage 5
  adds the codec-adapter policy table (`Adapter`: preset vocabulary, quality
  windows, per-codec argv shape), `AdapterEncoder`, and `ProbeSource`.
- **`pkg/bisect/`** — Stateless `Run(src, enc, scoreFunc, params)` function.
  The score function is injectable, enabling unit tests without a live `vmaf` binary.
  Stage 5 adds `YUVScoreFunc`, which decodes a containerised distorted file to
  raw YUV and invokes `vmaf` with full geometry / model / backend flags.
- **`pkg/ladder/`** — `Build(src, encoder, Params)` function. Convex hull
  (`upperConvexHull`), knee selection (`selectRenditions`), min-bitrate-gap filter.
- **`pkg/pershot/`** — Stage-5 shot detection, uniform-window splitter, per-shot
  tuning, and encoding-plan construction + JSON emission.
- **`pkg/scorebackend/`** — Stage-5 libvmaf backend resolution: parses the
  `vmaf --help` backend line, probes each vendor independently, and honours an
  explicit `--score-backend` strictly.
- **`pkg/report/`** — Stage-1: `EmitJSON` / `EmitMarkdown` renderers (single-run emit).
  Stage-4: `RenderMarkdownMulti` / `RenderHTMLMulti` (multi-file report rendering).

See [ADR-0705](../adr/0705-vmafx-tune-go-stage1.md) for the migration rationale,
[ADR-0730](../adr/0730-vmafx-tune-go-stage2.md) for Stage-2,
[ADR-0770](../adr/0770-vmafx-tune-go-stage4-report.md) for Stage-4, and
[ADR-0702](../adr/0702-vmafx-phase4-language-modernization.md) for the Phase 4 umbrella.
