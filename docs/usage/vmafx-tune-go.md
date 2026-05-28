# vmafx-tune-go — Go port of vmaf-tune (Stage 3)

`vmafx-tune-go` is the Stage-3 Go port of the `vmaf-tune` rate-quality tuning
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

### `ladder` — Per-title ABR bitrate-ladder (Stage 2 + Stage 3)

Builds an ABR bitrate ladder for a single source clip.

For each `(resolution, VMAF target)` cell in the sampling grid, a CRF bisect
finds the highest CRF whose measured VMAF meets the target. The resulting
`(bitrate, vmaf)` cloud is reduced to its **upper convex hull** (the Pareto
frontier), and a small set of **knee renditions** is selected from the hull
using curvature-based inflection detection.

**Stage-3 additions (ADR-0734):**

- **Resolution-aware downscaling** — the source is scaled to each rendition
  resolution (Lanczos) before encoding, so VMAF is measured at the actual
  playback resolution.
- **Concurrent grid sampling** — `--workers N` runs up to N
  encoder/scorer pairs in parallel, bounded by a semaphore.

```text
vmafx-tune-go ladder [flags]
```

**Required flags:**

| Flag               | Description                          |
|--------------------|--------------------------------------|
| `--reference`, `-r` | Path to the reference video file    |

**Optional flags:**

| Flag                | Default                    | Description                                                       |
|---------------------|----------------------------|-------------------------------------------------------------------|
| `--codec`, `-c`     | `libx264`                  | Encoder codec name.                                               |
| `--resolutions`     | `320x240,…,1920x1080`      | Comma-separated `WxH` resolution grid.                            |
| `--targets`, `-t`   | `75,85,95`                 | VMAF targets for the sampling grid.                               |
| `--output`, `-o`    | stdout                     | Output file path.                                                 |
| `--format`          | `json`                     | Output format: `json` or `markdown`.                              |
| `--ffmpeg`          | `ffmpeg`                   | Path to the ffmpeg binary.                                        |
| `--vmaf`            | `vmaf`                     | Path to the vmaf binary.                                          |
| `--work-dir`        | OS temp dir                | Temporary encode directory.                                       |
| `--crf-lo`          | `0`                        | Lower CRF bound (best quality).                                   |
| `--crf-hi`          | `0`                        | Upper CRF bound (0 = encoder default).                            |
| `--max-iter`        | `12`                       | Bisect iterations per grid cell.                                  |
| `--max-rungs`       | `6`                        | Max renditions from the hull.                                     |
| `--min-bitrate-gap` | `100`                      | Minimum kbps gap between renditions.                              |
| `--workers`         | `NumCPU/2` (max 8)         | Concurrent grid-sampling goroutines (0 = use default). *Stage-3* |

Hardware encoders (`h264_nvenc`, `hevc_nvenc`, `h264_qsv`, `h264_amf`, etc.)
are supported when present in the environment — pass the codec name via
`--codec`.

**Example — 4-worker parallel ladder:**

```bash
vmafx-tune-go ladder \
  --reference src.mp4 \
  --codec libx264 \
  --targets 75,85,95 \
  --resolutions 320x240,640x480,1280x720,1920x1080 \
  --workers 4 \
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

#### Go vs. Python behavioural differences (Stage 3)

| Aspect                        | Go (`vmafx-tune-go ladder`)                    | Python (`vmaf-tune ladder`)              |
|-------------------------------|------------------------------------------------|------------------------------------------|
| Resolution scaling            | Lanczos downscale per rendition (Stage-3)      | Downscales source per rendition          |
| Concurrent grid sampling      | `--workers N` semaphore-bounded (Stage-3)      | Thread pool via `concurrent.futures`     |
| Schema                        | `schema_version: 1` superset                   | Same field names, no `schema_version`    |
| Uncertainty-aware rung pruning | `pkg/conformal` ready; CLI wiring in Stage 4  | Available via `--uncertainty-threshold`  |

---

### `bisect` — Bitrate-domain binary search (Stage 3)

Find the minimum bitrate that achieves a target VMAF score for a fixed encoder,
using binary search in the bitrate domain.

Unlike `compare`/`ladder` (which bisect in the CRF domain), `bisect` encodes in
**VBR mode** (`-b:v`) at each probed bitrate. This directly answers the
question: *"what is the minimum bitrate budget that achieves VMAF X for this
clip with encoder Y?"*

```text
vmafx-tune-go bisect [flags]
```

**Required flags:**

| Flag               | Description                          |
|--------------------|--------------------------------------|
| `--reference`, `-r` | Path to the reference video file    |

**Optional flags:**

| Flag              | Default     | Description                                              |
|-------------------|-------------|----------------------------------------------------------|
| `--encoder`, `-e` | `libx264`   | Encoder codec name.                                      |
| `--target-vmaf`, `-t` | `90.0`  | Target VMAF score to achieve.                            |
| `--bitrate-min`   | `500`       | Lower bound of bitrate window (kbps).                    |
| `--bitrate-max`   | `10000`     | Upper bound of bitrate window (kbps).                    |
| `--tolerance`     | `50`        | Convergence threshold (kbps). Stop when hi−lo ≤ tolerance.|
| `--max-iter`      | `20`        | Maximum bisect iterations.                               |
| `--scale-width`   | `0`         | Downscale width before encoding (0 = no scaling).        |
| `--scale-height`  | `0`         | Downscale height before encoding (0 = no scaling).       |
| `--output`, `-o`  | stdout      | Output file path.                                        |
| `--format`        | `json`      | Output format: `json` or `markdown`.                     |
| `--ffmpeg`        | `ffmpeg`    | Path to the ffmpeg binary.                               |
| `--vmaf`          | `vmaf`      | Path to the vmaf binary.                                 |
| `--work-dir`      | OS temp dir | Directory for temporary encode outputs.                  |

**Example:**

```bash
vmafx-tune-go bisect \
  --reference src.mp4 \
  --encoder libsvtav1 \
  --target-vmaf 90 \
  --bitrate-min 500 \
  --bitrate-max 10000 \
  --tolerance 50 \
  --output bisect.json
```

#### Output schema (schema_version 1)

```json
{
  "schema_version": 1,
  "src": "src.mp4",
  "encoder": "libsvtav1",
  "target_vmaf": 90.0,
  "bitrate_min_kbps": 500,
  "bitrate_max_kbps": 10000,
  "tolerance_kbps": 50,
  "tool_version": "dev",
  "wall_time_ms": 3200,
  "best_bitrate_kbps": 4250,
  "best_vmaf_score": 90.4,
  "best_encode_time_ms": 1100,
  "converged": true,
  "iterations": 7,
  "samples": [
    { "bitrate_kbps": 500,  "vmaf_score": 71.2, "encode_time_ms": 320 },
    { "bitrate_kbps": 10000,"vmaf_score": 96.3, "encode_time_ms": 890 },
    { "bitrate_kbps": 5250, "vmaf_score": 91.8, "encode_time_ms": 610 }
  ]
}
```

`best_bitrate_kbps` is `-1` when no bitrate in the window achieves the target.
`converged` is `true` when the window narrowed to ≤ `tolerance_kbps`;
`false` when `max_iter` was reached first.

---

## Not yet ported (Stage 4+)

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

| Stage   | Scope                                                                         | Status       |
|---------|-------------------------------------------------------------------------------|--------------|
| Stage 1 | `compare` subcommand, libx264/libx265                                         | Shipped      |
| Stage 2 | `ladder` subcommand, hardware encoders (NVENC/QSV/AMF)                       | Shipped      |
| Stage 3 | Resolution-aware downscaling, `--workers`, `pkg/conformal`, `bisect` subcommand | **This PR** |
| Stage 4 | Conformal interval CLI wiring, `tune-per-shot`, `report`                      | Planned      |
| Stage 5 | Feature parity with Python; rename to `vmafx-tune`                            | Planned      |
| Stage 6 | Remove Python binary                                                          | Planned      |

## Architecture

The Go binary uses an **adapter pattern** with five core packages:

- **`pkg/encoder/`** — `Encoder` interface, software (libx264, libx265) and
  hardware (NVENC, QSV, AMF, SVT-AV1) encoder implementations. Each encoder
  shells out to `ffmpeg`; no `libavcodec` CGo dependency.
  Stage-3: `EncodeParams.ScaleWidth/ScaleHeight` injects a Lanczos scale filter.
- **`pkg/bisect/`** — Stateless `Run(src, enc, scoreFunc, params)` function.
  The score function is injectable, enabling unit tests without `vmaf` on PATH.
  Stage-3: `Params.ScaleWidth/ScaleHeight` forwarded to encoder.
- **`pkg/bitratesearch/`** — VBR-mode bitrate-domain binary search (Stage-3).
- **`pkg/conformal/`** — Split-conformal prediction intervals for VMAF scores
  (Stage-3, ADR-0734). CLI wiring deferred to Stage 4.
- **`pkg/report/`** — `EmitJSON` / `EmitMarkdown` renderers for `compare`.
  JSON output is schema-compatible with the Python `compare.py` v1/v2 payloads.
- **`pkg/ladder/`** — `Build(src, encoder, Params)` function, upper convex hull,
  Kneedle-style knee selection, and the `SamplerFn` seam.
  Stage-3: concurrent grid sampling via a `Workers`-sized semaphore channel.
  Tests inject a stub sampler; no `ffmpeg` or `vmaf` binary needed for
  `go test ./pkg/ladder/`.

See [ADR-0705](../adr/0705-vmafx-tune-go-stage1.md) for the Stage-1 rationale,
[ADR-0730](../adr/0730-vmafx-tune-go-stage2.md) for the Stage-2 ladder decision,
and [ADR-0702](../adr/0702-vmafx-phase4-language-modernization.md) for the
Phase 4 umbrella.
