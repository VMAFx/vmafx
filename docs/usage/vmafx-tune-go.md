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

| `tune-per-shot` | `vmaf-tune tune-per-shot` |
| `corpus` | `vmaf-tune corpus` |

| `benchmark` | `vmaf-tune benchmark` |
| `auto` | `vmaf-tune auto` |
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
| Stage 3 | Downscale plumbing | — | Merged |
| Stage 4 | `report` subcommand, Markdown + HTML rendering | ADR-0770 | Merged |
| golusoris | Migrate the CLI root + subcommands onto the golusoris `clikit` (cobra + fx) framework; `VMAFX_`-prefixed config + injected `slog` | ADR-1119 | Merged |
| ML-driven | `recommend`, `predict`, `recommend-saliency`, `prefilter`; codec-adapter registry, encode/score drivers, predictor, saliency pipeline, native TPE | — | **This PR** |
| Stage 5 | `tune-per-shot` subcommand, conformal CLI wiring | Planned | — |

| golusoris | Migrate the CLI root + subcommands onto the golusoris `clikit` (cobra + fx) framework; `VMAFX_`-prefixed config + injected `slog` | ADR-1119 | **This PR** |
| Stage 5 (corpus/sidecar) | `corpus` + `sidecar` subcommands; `pkg/codecadapter`, `pkg/corpus`, `pkg/predictor`, `pkg/sidecar`, `pkg/pyjson` | ADR-0703 / ADR-0704 | **This PR** |
| Stage 5 (per-shot) | `tune-per-shot` subcommand, conformal CLI wiring | Planned | — |
| Stage 6 | `fast` subcommand (requires ONNX Go binding) | Planned | — |

| Stage 5 | `tune-per-shot` subcommand: `pkg/pershot`, `pkg/scorebackend`, codec-adapter table, raw-YUV scorer | ADR-0705 | **This PR** |
| Stage 5b | `conformal` CLI wiring | Planned | — |
| Stage 6 | `fast` subcommand + `tune-per-shot --fast-nr` (both require an ONNX Go binding) | Planned | — |

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
  Each encoder shells out to `ffmpeg`; no `libavcodec` CGo dependency. Stage 5
  adds the codec-adapter policy table (`Adapter`: preset vocabulary, quality
  windows, per-codec argv shape), `AdapterEncoder`, and `ProbeSource`.

  Each encoder shells out to `ffmpeg`; no `libavcodec` CGo dependency.
  `EncodeParams.InputArgs` carries ffmpeg *input-side* options so raw-YUV
  sources (`-f rawvideo -pix_fmt -s -r`) and sample clips (input-side
  `-ss` / `-t`, for fast-seek) work; `EncodeParams.OutputPath` pins a
  deterministic destination and `EncodeResult.OutputSizeBytes` reports the
  encode size for size-over-duration bitrate maths.
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
