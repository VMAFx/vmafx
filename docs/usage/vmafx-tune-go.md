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

## Ported subcommands (Stage 5 — corpus + sidecar)

### `corpus` — Phase A grid sweep

Sweeps a `(preset, crf)` grid against one or more references, encodes each cell,
scores it against the reference with the libvmaf CLI, and writes one JSONL row
per `(source, preset, crf)` combination.

```text
vmafx-tune-go corpus [flags]
```

The JSONL schema (v3) is the API contract the Phase B target-VMAF bisect and the
Phase C per-title CRF predictor consume — see
[vmaf-tune.md](vmaf-tune.md#corpus-jsonl-schema) for the column
reference. The Go writer emits the same bytes the Python writer does, including
the bare `NaN` tokens CPython's `json` module produces for columns libvmaf did
not populate, so a corpus written by either binary is readable by the same
trainers.

**Required flags:**

| Flag | Description |
|------|-------------|
| `--source` | Reference video. Repeat for multiple sources. |
| `--width` | Rung target width in pixels. |
| `--height` | Rung target height in pixels. |
| `--preset` | Encoder preset. Repeat for multiple presets. |

**Source and encode flags:**

| Flag | Default | Description |
|------|---------|-------------|
| `--pix-fmt` | `yuv420p` | ffmpeg `pix_fmt` of the reference. |
| `--framerate` | `24` | Reference framerate. |
| `--duration` | `0` | Reference duration in seconds. Bounds the encode and the bitrate calculation; `0` means the full source. |
| `--encoder` | `libx264` | Codec adapter. Any registered adapter is accepted — see [vmaf-tune-codec-adapters.md](vmaf-tune-codec-adapters.md). |
| `--crf` | — | Quality value. Repeat for multiple cells. Required unless `--coarse-to-fine` derives the axis. |
| `--two-pass` | off | Run a 2-pass encode for codecs that support it (libx264 / libx265). Adapters without true 2-pass emit a one-line stderr warning and run single-pass. |
| `--sample-clip-seconds` | `0` | Encode and score only the centre N-second slice of each source. Encode time scales linearly with the slice; expect a 1–2 VMAF-point delta versus full-clip on diverse content. |
| `--encode-dir` | `.workingdir2/encodes` | Scratch directory for encodes. |
| `--keep-encodes` | off | Retain encoded outputs after scoring and record their paths in `encode_path`. |
| `--no-source-hash` | off | Skip `src_sha256`. Faster on huge YUVs; loses provenance. |

**Scoring flags:**

| Flag | Default | Description |
|------|---------|-------------|
| `--vmaf-model` | `vmaf_v0.6.1` | libvmaf model version string. |
| `--neg` | off | Use the VMAF NEG (No Enhancement Gain) variant. Use for codec A-vs-B comparisons; **not** for production monitoring — see [vmaf-neg.md](../metrics/vmaf-neg.md). |
| `--score-backend` | `auto` | libvmaf backend: `auto`, `cpu`, `cuda`, `sycl`, `hip`. `auto` picks the fastest available (cuda > sycl > hip > cpu); a specific name is honoured strictly and errors out when unavailable. |
| `--ffmpeg-bin` | `ffmpeg` | Path to the ffmpeg binary. |
| `--vmaf-bin` | `vmaf` | Path to the vmaf binary. |
| `--ffprobe-bin` | `ffprobe` | Path to the ffprobe binary (used for HDR detection). |
| `--output` | `corpus.jsonl` | JSONL output path. |

**Search-mode flags:**

| Flag | Default | Description |
|------|---------|-------------|
| `--coarse-to-fine` | off | Run a 2-pass coarse-then-fine CRF search instead of the full grid. With the defaults that is 15 encodes rather than 52 — see [vmaf-tune-coarse-to-fine.md](vmaf-tune-coarse-to-fine.md). |
| `--coarse-step` | `10` | CRF step for the coarse pass. |
| `--fine-radius` | `5` | ± radius around the best-coarse CRF for the fine pass. |
| `--fine-step` | `1` | CRF step for the fine pass. |
| `--target-vmaf` | unset | Target VMAF. The search refines around the smallest CRF whose score meets it; without a target it refines around the highest-VMAF coarse point. |

**HDR flags** (mutually exclusive — see
[vmaf-tune-hdr-and-sampling.md](vmaf-tune-hdr-and-sampling.md)):

| Flag | Description |
|------|-------------|
| `--auto-hdr` | *(default)* Probe each source with ffprobe and inject HDR codec args when PQ / HLG signalling is detected. |
| `--force-sdr` | Treat every source as SDR; skip detection and flag injection. |
| `--force-hdr-pq` | Treat every source as HDR PQ (SMPTE-2084) regardless of the probe. |
| `--force-hdr-hlg` | Treat every source as HDR HLG (ARIB STD-B67) regardless of the probe. |

**Example — full grid:**

```bash
vmafx-tune-go corpus \
  --source ref.yuv \
  --width 1920 --height 1080 \
  --framerate 24 --duration 10 \
  --preset medium \
  --crf 20 --crf 26 --crf 32 \
  --output corpus.jsonl
```

**Example — coarse-to-fine against a VMAF target:**

```bash
vmafx-tune-go corpus \
  --source ref.yuv \
  --width 1920 --height 1080 \
  --preset medium \
  --coarse-to-fine --target-vmaf 93 \
  --output corpus.jsonl
```

Rows stream to the output file as each cell completes, so an interrupted sweep
leaves a usable partial corpus. The selected scoring backend is echoed on
`stderr` (`vmafx-tune: scoring backend = cpu`) before the first encode, and the
row count is echoed when the sweep finishes.

### `sidecar` — Local on-host predictor sidecar

Trains and inspects the local bias-correction model described in
[local-sidecar-training.md](../ai/local-sidecar-training.md). The shipped
predictor is a fixed, deterministic asset; the sidecar is a correction term you
train on your own host from the residuals between predicted VMAF and the
libvmaf score actually observed at encode time:

```text
sidecar_vmaf = predictor_vmaf + sidecar_correction
```

State lives under
`${XDG_CACHE_HOME:-~/.cache}/vmaf-tune/sidecar/<predictor-version>/<codec>/state.json`
alongside an anonymous random host UUID. A predictor-version mismatch on load
discards everything except the UUID and resets to cold start, so a shipped-model
upgrade can never replay a stale correction. The on-disk format is shared with
the Python implementation: state written by one binary loads in the other and
produces identical predictions.

**Shared flags** (accepted by every child command):

| Flag | Default | Description |
|------|---------|-------------|
| `--codec` | `libx264` | Codec bucket for the sidecar state. |
| `--cache-dir` | `${XDG_CACHE_HOME:-~/.cache}/vmaf-tune/sidecar` | Sidecar cache root. |
| `--predictor-version` | `predictor_v1` | Predictor version namespace. |
| `--model` | unset | Optional `predictor_<codec>.onnx` path. **Not supported by the Go binary** — see the ONNX note below. |
| `--json` | off | Emit machine-readable JSON (2-space indent, sorted keys) instead of the one-line text summary. |

#### `sidecar status`

Prints the state metadata for one codec bucket: the anonymous host UUID, the
on-disk state path, the predictor-version namespace, the number of folded-in
captures, and the RMS of the buffered residuals (the drift signal).

```bash
vmafx-tune-go sidecar status --codec libx264 --json
```

```json
{
  "codec": "libx264",
  "host_uuid": "0123456789abcdef0123456789abcdef",
  "n_updates": 0,
  "predictor_version": "predictor_v1",
  "recent_residual_rms": 0.0,
  "schema": "vmaf-tune-sidecar-status/v1",
  "schema_version": 1,
  "state_path": "/home/u/.cache/vmaf-tune/sidecar/predictor_v1/libx264/state.json"
}
```

#### `sidecar predict`

Predicts VMAF for one shot at one CRF with the correction applied.

| Flag | Description |
|------|-------------|
| `--features-json` | *(required)* Path to a JSON object carrying the shot's feature values. |
| `--crf` | *(required)* CRF to predict at. |

```bash
vmafx-tune-go sidecar predict --features-json shot.json --crf 26 --json
```

The payload carries `base_vmaf` (the bare predictor), `correction`, and
`sidecar_vmaf` (the sum, clamped to `[0, 100]`).

#### `sidecar record`

Folds one observed VMAF measurement into the ridge fit.

| Flag | Description |
|------|-------------|
| `--features-json` | *(required)* Path to the shot's feature JSON. |
| `--crf` | *(required)* CRF the observation was measured at. |
| `--observed-vmaf` | *(required)* Observed libvmaf score for the encode. |
| `--no-persist` | Update in memory only; mainly useful for tests. |

```bash
vmafx-tune-go sidecar record \
  --features-json shot.json \
  --crf 26 \
  --observed-vmaf 91.75 \
  --json
```

The residual is computed against the **bare** predictor, never against the
sidecar-corrected value, so repeated captures converge rather than compounding.

#### `sidecar batch-record`

Folds a whole JSONL capture file into the fit, one observation per line.

| Flag | Description |
|------|-------------|
| `--captures-jsonl` | *(required)* Path to the JSONL capture file. |

```bash
vmafx-tune-go sidecar batch-record --captures-jsonl captures.jsonl --json
```

Each line is a JSON object carrying the feature fields (either at the top level
or nested under a `features` key) plus `crf` and `observed_vmaf`. Malformed rows
are skipped with a one-line diagnostic on `stderr` and counted in
`rows_skipped`; the run still succeeds, so one truncated line does not discard a
long capture session. State is written once at the end rather than per row.

#### Feature-JSON shape

Both `--features-json` and each `--captures-jsonl` row accept the same object.
The four `probe_*` fields are **required** — a zero probe bitrate would train
the fit on a fabricated complexity barometer — and everything else defaults to
zero:

```json
{
  "probe_bitrate_kbps": 4200.5,
  "probe_i_frame_avg_bytes": 51234.0,
  "probe_p_frame_avg_bytes": 8123.25,
  "probe_b_frame_avg_bytes": 2011.75,
  "saliency_mean": 0.42,
  "saliency_var": 0.031,
  "frame_diff_mean": 7.5,
  "y_avg": 112.25,
  "y_var": 1830.5,
  "shot_length_frames": 240,
  "fps": 24.0,
  "width": 1920,
  "height": 1080
}
```

#### ONNX note (`--model`)

The Python `Predictor` loads a learned `predictor_<codec>.onnx` through
`onnxruntime` when `--model` is set, and silently falls back to the analytical
curve when `onnxruntime` is not importable. The Go binary has **no in-process
ONNX runtime**, so `--model` is rejected outright rather than silently scored
against a different model than you asked for: a wrong-but-plausible VMAF number
is worse than a clear refusal.

Omit `--model` to use the analytical fallback — the default, and the path the
sidecar operator surface exercises in practice — or run `vmaf-tune sidecar` for
the ONNX path. Every other behaviour, including the persisted state format, is
identical between the two binaries.

## Not yet ported

The following subcommands are stubs in `vmafx-tune-go`. They log a redirect
notice (a `WARN`-level structured log line) and exit 1 when invoked. Use the
Python `vmaf-tune` binary for these:

| Subcommand | Python equivalent |
|------------|-------------------|
| `tune-per-shot` | `vmaf-tune tune-per-shot` |
| `fast` | `vmaf-tune fast` |
| `benchmark` | `vmaf-tune benchmark` |
| `auto` | `vmaf-tune auto` |
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
| Stage 5 (corpus/sidecar) | `corpus` + `sidecar` subcommands; `pkg/codecadapter`, `pkg/corpus`, `pkg/predictor`, `pkg/sidecar`, `pkg/pyjson` | ADR-0703 / ADR-0704 | **This PR** |
| Stage 5 (per-shot) | `tune-per-shot` subcommand, conformal CLI wiring | Planned | — |
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

The `corpus` and `sidecar` subcommands add five more:

- **`pkg/codecadapter/`** — the ADR-0237 adapter registry: every codec's quality
  knob, preset vocabulary, validation rule, and ffmpeg argv slice. The encode
  driver never branches on codec identity.
- **`pkg/corpus/`** — the Phase A orchestrator: encode + score drivers, the
  pass-1 encoder-stats parser, HDR detection and codec-arg dispatch, shot
  metadata, scoring-backend selection, the JSONL reader / writer, and the
  coarse-to-fine search.
- **`pkg/predictor/`** — `ShotFeatures` plus the per-codec analytical VMAF curve
  and its CRF inversion.
- **`pkg/sidecar/`** — the online-ridge bias-correction model (Sherman-Morrison
  rank-1 updates) and its cache-dir persistence.
- **`pkg/pyjson/`** — a CPython-compatible JSON encoder. The corpus JSONL and the
  sidecar `--json` payloads are cross-implementation artefacts, so the writer
  reproduces `json.dumps` byte-for-byte: bare `NaN` / `Infinity` tokens,
  `repr()`-style float rendering, and `ensure_ascii` escaping. `pkg/corpus` also
  ports CPython's Neumaier-compensated `sum()` and `statistics.pstdev()` so the
  aggregate columns match to the last bit.

See [ADR-0705](../adr/0705-vmafx-tune-go-stage1.md) for the migration rationale,
[ADR-0730](../adr/0730-vmafx-tune-go-stage2.md) for Stage-2,
[ADR-0770](../adr/0770-vmafx-tune-go-stage4-report.md) for Stage-4, and
[ADR-0702](../adr/0702-vmafx-phase4-language-modernization.md) for the Phase 4 umbrella.
