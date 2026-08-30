<!-- markdownlint-disable MD013 MD060 -->
# vmafx-tune-go — Go port of vmaf-tune (Stage 4 + encoder introspection)

`vmafx-tune-go` is the Go port of the `vmaf-tune` rate-quality tuning CLI
(Stages 1–4, plus the encoder-introspection subcommands). It ships as a
**separate binary** alongside the Python `vmaf-tune` binary during the
migration. The Python binary is unchanged and should be used for all
subcommands that are not yet ported.

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

## Ported subcommands (encoder introspection)

### `benchmark` — Rank encoders from an existing corpus

Answers the standard post-sweep question: *which encoder hit the target quality
at the lowest bitrate?* It reads a Phase-A corpus JSONL written by
`vmaf-tune corpus` and launches **no** ffmpeg and no libvmaf — the corpus stays
the source of truth.

```text
vmafx-tune-go benchmark --from-corpus <JSONL> [flags]
```

For every encoder in the corpus the report picks the lowest-bitrate row whose
measured VMAF clears `--target-vmaf`. An encoder that never clears is reported
with status `unmet` and its closest miss, so a missing encoder build is never
mistaken for a quality result.

**Required flags:**

| Flag | Description |
|------|-------------|
| `--from-corpus` | Phase-A corpus JSONL to benchmark |

**Optional flags:**

| Flag | Default | Description |
|------|---------|-------------|
| `--target-vmaf` | `92` | Matched-quality threshold each encoder must clear. |
| `--baseline-encoder` | lowest-bitrate encoder that clears | Encoder used for the `bitrate_delta_pct` column. |
| `--format` | `markdown` | Report format: `markdown`, `json` or `csv`. |
| `--output`, `-o` | stdout | Report destination. Parent directories are created. |

**Example — Markdown to stdout:**

```bash
vmafx-tune-go benchmark --from-corpus corpus.jsonl
```

**Example — CSV at a stricter target, with a pinned baseline:**

```bash
vmafx-tune-go benchmark \
  --from-corpus corpus.jsonl \
  --target-vmaf 95 \
  --baseline-encoder libx264 \
  --format csv \
  --output benchmark.csv
```

Rows are ranked with cleared encoders first (ascending bitrate), then the
`unmet` ones. A row reports:

| Field | Meaning |
|-------|---------|
| `encoder` | Encoder token from the corpus rows. |
| `status` | `ok` when the encoder cleared the target, `unmet` otherwise. |
| `target_vmaf` / `margin` | The requested threshold, and the selected row's VMAF minus it (negative when `unmet`). |
| `bitrate_kbps` | Bitrate of the selected row. |
| `bitrate_delta_pct` | Percentage difference against the baseline encoder; `null` / blank when no encoder cleared. |
| `rows` / `source_count` / `preset_count` | How many eligible corpus rows the encoder contributed, and how many distinct sources / presets they span. |
| `encode_fps` / `score_fps` | Means over the encoder's rows, counting positive finite samples only; `null` / blank when no row supplied timings. |
| `best` | The selected corpus row (`src`, `preset`, `crf`, `vmaf_score`, `bitrate_kbps`, `vmaf_model`). |

Rows are excluded from the report when `exit_status` is non-zero, when
`vmaf_score` or `bitrate_kbps` is missing or non-finite, or when the row
carries no encoder name.

The CSV output uses CRLF line endings (Python's `csv` "excel" dialect), and the
JSON output is stable pretty RFC 8259 with sorted keys — both byte-identical to
`vmaf-tune benchmark`.

### `encode-profile` — Reproduce one recommendation from a report

Every vmaf-tune report embeds a machine-readable `encoder_profile` payload.
This subcommand reads that payload, selects one recommendation, and runs the
matching FFmpeg encode.

```text
vmafx-tune-go encode-profile --profile <FILE> --output <FILE> [flags]
```

The profile is accepted in any of the three shapes a report ships in:

| Input | How the payload is found |
|-------|--------------------------|
| Report JSON (`.json`) | Parsed directly; the `encoder_profile` block is unwrapped if present. |
| Report HTML (`.html` / `.htm`) | Extracted from the raw-JSON `<pre>` block and HTML-unescaped. |
| Report Markdown (anything else) | Extracted from the fenced JSON payload. |

Selection defaults to the first Pareto-selected row with the lowest bitrate.
`--codec` and `--target-vmaf` narrow the candidate set; `--recommendation-index`
then picks the Nth survivor (zero-based, applied **after** filtering).

**Required flags:**

| Flag | Description |
|------|-------------|
| `--profile` | Report JSON / HTML / Markdown containing `encoder_profile` |
| `--output`, `-o` | Encoded output path |

**Selection flags:**

| Flag | Default | Description |
|------|---------|-------------|
| `--codec` | all | Restrict selection to one codec. |
| `--target-vmaf` | all | Restrict selection to one target VMAF (matched with a 1e-6 absolute tolerance). |
| `--recommendation-index` | `0` (first) | Zero-based index after the filters are applied. |

**Override flags** (each falls back to the profile value when omitted):

| Flag | Description |
|------|-------------|
| `--src` | Override the source path stored in the profile. |
| `--preset` | Override the stored / adapter-default preset. |
| `--pix-fmt` | Override the raw-source pixel format (default `yuv420p`). |
| `--framerate`, `--width`, `--height` | Override the raw-source geometry. |
| `--duration` | Override the encode duration in seconds. Passing `0` explicitly suppresses the profile's own duration bound. |
| `--source-kind` | `auto` (default), `container` or `raw`. Under `auto`, `.yuv` / `.raw` / `.rgb` / `.gray` are raw and everything else is a container. |
| `--sample-clip-seconds`, `--sample-clip-start-s` | Input-side clip length / offset forwarded to FFmpeg. |
| `--extra-ffmpeg-arg` | Append one raw FFmpeg argv token after the codec args; repeat as needed. Use `--extra-ffmpeg-arg=-movflags` for tokens starting with `-`. |
| `--ffmpeg-bin` | Override the profile's `ffmpeg_bin` (default: profile value, then `ffmpeg`). |
| `--dry-run` | Print the selected recommendation and the exact FFmpeg argv without encoding. |

**Example — inspect the selection without encoding:**

```bash
vmafx-tune-go encode-profile \
  --profile report.json \
  --output /dev/null \
  --dry-run
```

**Example — reproduce the best x265 row at target 95:**

```bash
vmafx-tune-go encode-profile \
  --profile report.html \
  --codec libx265 \
  --target-vmaf 95 \
  --output encoded.mkv
```

**Output schema.** The result is a single JSON document on `stdout` (sorted
keys, two-space indent). A `--dry-run` emits `ok`, `dry_run`, `profile`,
`selected`, `ffmpeg_argv` and `output`; a real run replaces `dry_run` with the
encode outcome:

```json
{
  "ok": true,
  "profile": "report.json",
  "selected": { "codec": "libx265", "crf": 28 },
  "ffmpeg_argv": ["ffmpeg", "-y", "-hide_banner"],
  "output": "encoded.mkv",
  "exit_status": 0,
  "encode_size_bytes": 148213,
  "encode_time_ms": 1843.2,
  "encoder_version": "libx265-3.5",
  "ffmpeg_version": "n8.1",
  "stderr_tail": "..."
}
```

**Exit status.** On a real run the process exit status is FFmpeg's own, so a
failed encode surfaces the encoder's code (for example `254`) rather than a
generic `1`. The JSON payload is still written first, so a wrapper script can
read `exit_status` and `stderr_tail` regardless.

### Exit codes

`benchmark` and `encode-profile` use the same exit-status convention as the
Python CLI they replace:

| Status | Meaning |
|--------|---------|
| `0` | Success. |
| `2` | A usage or validation failure — a missing/unknown flag, an unparseable flag value, a missing input file, a filter that matches no recommendation, a baseline encoder absent from the corpus. |
| other | `encode-profile` only: FFmpeg's own exit status from a failed encode. |

Note that the earlier ports (`compare`, `ladder`, `report`) still report every
failure as `1`; that pre-existing inconsistency is tracked separately.

**Hardware-encoder caveats.**

- The emitted argv contains **no** `-init_hw_device` chain. FFmpeg's QSV bridge
  on Linux needs `-init_hw_device vaapi=va:<node> -init_hw_device qsv=qsv_dev@va
  -filter_hw_device va` before the first `-i`, plus a `format=nv12,hwupload`
  filter, or the encode fails with `-22` (see
  [ADR-0601](../adr/0601-vmaftune-qsv-amf-hw-init-and-probe-fix.md)). This
  matches the Python implementation exactly: `vmaf-tune` injects that chain in
  its `compare` sweep, never in `encode-profile`. Supply the flags yourself
  with repeated `--extra-ffmpeg-arg`, or drive QSV through `compare`.
- Hardware encoders (NVENC / QSV / AMF) reject sources below roughly
  **320x240**. A profile built from a smaller clip will fail at the encoder.
- `av1_videotoolbox` is a placeholder: upstream FFmpeg ships no such encoder,
  so the adapter refuses to emit an argv shape it cannot verify
  ([ADR-0339](../adr/0339-av1-videotoolbox-placeholder-adapter.md)).

## Not yet ported (Stage 5+)

The following subcommands are stubs in `vmafx-tune-go`. They log a redirect
notice (a `WARN`-level structured log line) and exit 1 when invoked. Use the
Python `vmaf-tune` binary for these:

| Subcommand | Python equivalent |
|------------|-------------------|
| `tune-per-shot` | `vmaf-tune tune-per-shot` |
| `fast` | `vmaf-tune fast` |
| `corpus` | `vmaf-tune corpus` |
| `auto` | `vmaf-tune auto` |
| `sidecar` | `vmaf-tune sidecar` |

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
| Encoder introspection | `benchmark` + `encode-profile` subcommands; `pkg/benchmark`, `pkg/codecadapter`, `pkg/encodeprofile`, `internal/pyjson` | ADR-0770 | **This PR** |
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

The encoder-introspection subcommands add four more:

- **`pkg/benchmark/`** — corpus loading (`LoadCorpusJSONL`), per-encoder
  summarisation (`Summarize`) and the three renderers. No subprocess at all.
- **`pkg/codecadapter/`** — the argv-shaping half of `vmaftune.codec_adapters`:
  19 codecs, each answering "what is the `-c:v ...` slice for this
  (preset, quality)?" so nothing else branches on codec identity.
- **`pkg/encodeprofile/`** — profile loading from JSON / HTML / Markdown,
  recommendation selection, `EncodeRequest` construction, FFmpeg argv
  composition and the encode driver (with an injectable `Runner` seam so tests
  never spawn ffmpeg).
- **`internal/pyjson/`** — renders Go value trees byte-identically to CPython's
  `json.dumps(..., indent=2, sort_keys=True)`. Go's `encoding/json` differs on
  key ordering, HTML escaping, non-ASCII escaping and float formatting
  (`float64(92)` renders `92` in Go and `92.0` in CPython), so a shared encoder
  keeps the ported payloads diff-clean against the Python originals.

See [ADR-0705](../adr/0705-vmafx-tune-go-stage1.md) for the migration rationale,
[ADR-0730](../adr/0730-vmafx-tune-go-stage2.md) for Stage-2,
[ADR-0770](../adr/0770-vmafx-tune-go-stage4-report.md) for Stage-4, and
[ADR-0702](../adr/0702-vmafx-phase4-language-modernization.md) for the Phase 4 umbrella.
