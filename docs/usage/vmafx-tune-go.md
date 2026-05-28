# vmafx-tune-go — Go port of vmaf-tune (Stage 1)

`vmafx-tune-go` is the Stage-1 Go port of the `vmaf-tune` rate-quality tuning
CLI. It ships as a **separate binary** alongside the Python `vmaf-tune` binary
during the migration. The Python binary is unchanged and should be used for all
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

```
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

## Not yet ported (Stage 2+)

The following subcommands are stubs in `vmafx-tune-go`. They print a redirect
message and exit 1 when invoked. Use the Python `vmaf-tune` binary for these:

| Subcommand | Python equivalent |
|------------|-------------------|
| `tune-per-shot` | `vmaf-tune tune-per-shot` |
| `ladder` | `vmaf-tune ladder` |
| `fast` | `vmaf-tune fast` |
| `corpus` | `vmaf-tune corpus` |
| `report` | `vmaf-tune report` |
| `benchmark` | `vmaf-tune benchmark` |
| `auto` | `vmaf-tune auto` |
| `sidecar` | `vmaf-tune sidecar` |
| `encode-profile` | `vmaf-tune encode-profile` |

## Migration roadmap

| Stage | Scope | Status |
|-------|-------|--------|
| Stage 1 | `compare` subcommand, libx264/libx265, single/multi-target bisect | **This PR** |
| Stage 2 | Hardware encoders (NVENC, QSV, AMF), `--workers`, `ladder`, `tune-per-shot` | Planned |
| Stage 3 | Feature parity with Python binary; rename to `vmafx-tune` | Planned |
| Stage 4 | Remove Python binary | Planned |

## Architecture

The Go binary uses an **adapter pattern** with three core packages:

- **`pkg/encoder/`** — `Encoder` interface + software encoder implementations.
  Each encoder shells out to `ffmpeg` for the actual encode; no `libavcodec` CGo
  dependency in Stage 1.
- **`pkg/bisect/`** — Stateless `Run(src, enc, scoreFunc, params)` function.
  The score function is injectable, enabling unit tests without a live `vmaf` binary.
- **`pkg/report/`** — `EmitJSON` / `EmitMarkdown` renderers. JSON output is
  schema-compatible with the Python `compare.py` v1/v2 payloads.

See [ADR-0705](../adr/0705-vmafx-tune-go-stage1.md) for the migration rationale and
[ADR-0702](../adr/0702-vmafx-phase4-language-modernization.md) for the Phase 4
umbrella.
