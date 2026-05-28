# vmafx-tune-go — Go port of vmaf-tune (Stage 2)

`vmafx-tune-go` is the Stage-2 Go port of the `vmaf-tune` rate-quality tuning
CLI. It ships as a **separate binary** alongside the Python `vmaf-tune` binary
during the migration. The Python binary is unchanged and should be used for all
subcommands that are not yet ported.

This page documents the Go binary. For the full Python `vmaf-tune` reference,
see [vmaf-tune.md](vmaf-tune.md).

## Build

```bash
go build -o vmafx-tune-go ./cmd/vmafx-tune
# or with version injection:
go build -ldflags "-X main.version=$(cat VERSION)" \
  -o vmafx-tune-go ./cmd/vmafx-tune
```

## Ported subcommands

### `compare` — Rate-quality sweep

Runs a VMAF-target bisect for each `(codec, target)` pair and emits a ranked
report.

```text
vmafx-tune-go compare [flags]
```

**Required flags:**

| Flag               | Description                          |
|--------------------|--------------------------------------|
| `--reference`, `-r` | Path to the reference video file    |

**Optional flags:**

| Flag          | Default           | Description                                   |
|---------------|-------------------|-----------------------------------------------|
| `--codecs`, `-c` | `libx264,libx265` | Comma-separated encoder names.             |
| `--targets`, `-t` | `85`           | Comma-separated VMAF target(s).               |
| `--output`, `-o` | stdout          | Output file path.                             |
| `--format`    | `json`            | Output format: `json` or `markdown`.          |
| `--ffmpeg`    | `ffmpeg`          | Path to the ffmpeg binary.                    |
| `--vmaf`      | `vmaf`            | Path to the vmaf binary for scoring.          |
| `--work-dir`  | OS temp dir       | Directory for temporary encode outputs.       |
| `--crf-lo`    | `0`               | Lower bound of CRF search window.             |
| `--crf-hi`    | `0`               | Upper bound (0 = encoder default: 51).        |
| `--max-iter`  | `12`              | Maximum bisect iterations per pair.           |

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

#### Output schema

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
      "bisect_samples": []
    }
  ]
}
```

Multi-target output uses schema-v2 (adds `schema_version: 2` and
`target_vmafs: [...]`), also compatible with the Python `report.py` renderer.

Non-finite float values (`NaN`, `Inf`) are serialized as `null` — the output
is RFC 8259 strict JSON, parseable by Go, Rust, and `jq --strict`.

---

### `ladder` — Per-title ABR bitrate-ladder (Stage 2)

Builds an ABR bitrate ladder for a single source clip.

For each `(resolution, VMAF target)` cell in the sampling grid, a CRF bisect
finds the highest CRF whose measured VMAF meets the target. The resulting
`(bitrate, vmaf)` cloud is reduced to its **upper convex hull** (the Pareto
frontier), and a small set of **knee renditions** is selected from the hull
using curvature-based inflection detection.

```text
vmafx-tune-go ladder [flags]
```

**Required flags:**

| Flag               | Description                          |
|--------------------|--------------------------------------|
| `--reference`, `-r` | Path to the reference video file    |

**Optional flags:**

| Flag                | Default                    | Description                              |
|---------------------|----------------------------|------------------------------------------|
| `--codec`, `-c`     | `libx264`                  | Encoder codec name.                      |
| `--resolutions`     | `320x240,…,1920x1080`      | Comma-separated `WxH` resolution grid.   |
| `--targets`, `-t`   | `75,85,95`                 | VMAF targets for the sampling grid.      |
| `--output`, `-o`    | stdout                     | Output file path.                        |
| `--format`          | `json`                     | Output format: `json` or `markdown`.     |
| `--ffmpeg`          | `ffmpeg`                   | Path to the ffmpeg binary.               |
| `--vmaf`            | `vmaf`                     | Path to the vmaf binary.                 |
| `--work-dir`        | OS temp dir                | Temporary encode directory.              |
| `--crf-lo`          | `0`                        | Lower CRF bound (best quality).          |
| `--crf-hi`          | `0`                        | Upper CRF bound (0 = encoder default).   |
| `--max-iter`        | `12`                       | Bisect iterations per grid cell.         |
| `--max-rungs`       | `6`                        | Max renditions from the hull.            |
| `--min-bitrate-gap` | `100`                      | Minimum kbps gap between renditions.     |

Hardware encoders (`h264_nvenc`, `hevc_nvenc`, `h264_qsv`, `h264_amf`, etc.)
are supported when present in the environment — pass the codec name via
`--codec`.

**Example:**

```bash
vmafx-tune-go ladder \
  --reference src.mp4 \
  --codec libx264 \
  --targets 75,85,95 \
  --resolutions 320x240,640x480,1280x720,1920x1080 \
  --output ladder.json
```

#### Output schema

```json
{
  "schema_version": 1,
  "src": "src.mp4",
  "encoder": "libx264",
  "target_vmafs": [75.0, 85.0, 95.0],
  "resolutions": ["320x240", "640x480", "1280x720", "1920x1080"],
  "tool_version": "dev",
  "wall_time_ms": 18400,
  "cloud": [
    { "width": 320, "height": 240, "bitrate_kbps": 280.0,
      "vmaf": 75.3, "crf": 34, "target_vmaf": 75.0, "ok": true }
  ],
  "hull": [ ],
  "renditions": [
    { "width": 320,  "height": 240,  "bitrate_kbps": 280.0,
      "vmaf": 75.3,  "crf": 34 },
    { "width": 1920, "height": 1080, "bitrate_kbps": 4100.0,
      "vmaf": 95.1,  "crf": 18 }
  ]
}
```

`cloud` — every sampled `(resolution, VMAF-target)` point, including failures.\
`hull` — the upper convex hull (Pareto frontier), sorted by ascending bitrate.\
`renditions` — selected knee points from the hull, capped at `--max-rungs`.

#### Go vs. Python behavioural differences

| Aspect                        | Go (`vmafx-tune-go ladder`)         | Python (`vmaf-tune ladder`)              |
|-------------------------------|--------------------------------------|------------------------------------------|
| Resolution scaling            | Not applied (Stage-2 scope)          | Downscales source per rendition          |
| Concurrent grid sampling      | Sequential (Stage-3 adds `--workers`)| Thread pool via `concurrent.futures`     |
| Schema                        | `schema_version: 1` superset         | Same field names, no `schema_version`    |
| Uncertainty-aware rung pruning | Not implemented (Stage-3 scope)     | Available via `--uncertainty-threshold`  |

---

## Not yet ported (Stage 3+)

The following subcommands are stubs in `vmafx-tune-go`. They print a redirect
message and exit 1 when invoked. Use the Python `vmaf-tune` binary for these:

| Subcommand      | Python equivalent              |
|-----------------|-------------------------------|
| `tune-per-shot` | `vmaf-tune tune-per-shot`     |
| `fast`          | `vmaf-tune fast`              |
| `corpus`        | `vmaf-tune corpus`            |
| `report`        | `vmaf-tune report`            |
| `benchmark`     | `vmaf-tune benchmark`         |
| `auto`          | `vmaf-tune auto`              |
| `sidecar`       | `vmaf-tune sidecar`           |
| `encode-profile` | `vmaf-tune encode-profile`   |

## Migration roadmap

| Stage   | Scope                                               | Status       |
|---------|-----------------------------------------------------|--------------|
| Stage 1 | `compare` subcommand, libx264/libx265               | Shipped      |
| Stage 2 | `ladder` subcommand, hardware encoders (NVENC/QSV/AMF) | **This PR** |
| Stage 3 | `tune-per-shot`, resolution-scaling, `--workers`    | Planned      |
| Stage 4 | Feature parity with Python; rename to `vmafx-tune`  | Planned      |
| Stage 5 | Remove Python binary                                | Planned      |

## Architecture

The Go binary uses an **adapter pattern** with four core packages:

- **`pkg/encoder/`** — `Encoder` interface, software (libx264, libx265) and
  hardware (NVENC, QSV, AMF, SVT-AV1) encoder implementations. Each encoder
  shells out to `ffmpeg`; no `libavcodec` CGo dependency.
- **`pkg/bisect/`** — Stateless `Run(src, enc, scoreFunc, params)` function.
  The score function is injectable, enabling unit tests without `vmaf` on PATH.
- **`pkg/report/`** — `EmitJSON` / `EmitMarkdown` renderers for `compare`.
  JSON output is schema-compatible with the Python `compare.py` v1/v2 payloads.
- **`pkg/ladder/`** — `Build(src, encoder, Params)` function, upper convex hull,
  Kneedle-style knee selection, and the `SamplerFn` seam.
  Tests inject a stub sampler; no `ffmpeg` or `vmaf` binary needed for
  `go test ./pkg/ladder/`.

See [ADR-0705](../adr/0705-vmafx-tune-go-stage1.md) for the Stage-1 rationale,
[ADR-0730](../adr/0730-vmafx-tune-go-stage2.md) for the Stage-2 ladder decision,
and [ADR-0702](../adr/0702-vmafx-phase4-language-modernization.md) for the
Phase 4 umbrella.
