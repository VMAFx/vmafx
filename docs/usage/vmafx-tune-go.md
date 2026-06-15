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
| golusoris | Migrate the CLI root + subcommands onto the golusoris `clikit` (cobra + fx) framework; `VMAFX_`-prefixed config + injected `slog` | ADR-1119 | **This PR** |
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

See [ADR-0705](../adr/0705-vmafx-tune-go-stage1.md) for the migration rationale,
[ADR-0730](../adr/0730-vmafx-tune-go-stage2.md) for Stage-2,
[ADR-0770](../adr/0770-vmafx-tune-go-stage4-report.md) for Stage-4, and
[ADR-0702](../adr/0702-vmafx-phase4-language-modernization.md) for the Phase 4 umbrella.
