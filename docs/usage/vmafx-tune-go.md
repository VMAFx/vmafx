<!-- markdownlint-disable MD013 MD060 -->
# vmafx-tune-go — Go port of vmaf-tune

`vmafx-tune-go` is the Go port of the `vmaf-tune` rate-quality tuning CLI: it
finds the encoder settings (CRF, preset, codec) that hit a VMAF target at the
lowest bitrate. All fourteen `vmaf-tune` subcommands are ported: `compare`,
`ladder`, `report`, `recommend`, `predict`, `recommend-saliency`, `prefilter`,
`tune-per-shot`, `fast`, `corpus`, `sidecar`, `benchmark`, `encode-profile` and
`auto`. It is the active tuning binary after the retirement of the Python CLI
shadow.

This page documents the Go binary. For the overview of the tuning workflow and
the Python `vmaf-tune` CLI, start at [vmaf-tune.md](vmaf-tune.md). A few
individual flags still require Python (see [Python-only
flags](#python-only-flags)),
and the flag spellings of the three oldest ports differ from Python (see
[Python vs Go differences](#python-vs-go-differences)).

## Build

Go 1.27 or newer is required (`GO_VERSION` in `build-config.env`; `go.mod`
declares `go 1.27.2`).

```bash
go build -o vmafx-tune-go ./cmd/vmafx-tune
# or with version injection:
go build -ldflags "-X main.version=$(cat VERSION)" -o vmafx-tune-go ./cmd/vmafx-tune
```

The binary installs as `vmafx-tune-go`, not `vmaf-tune`, so it never collides
with the Python entry point. The dev container builds it with
`go build ./cmd/...`
and ships it as `vmafx-tune`; the package is pure Go and builds without cgo.

External tools used at run time:

| Tool | Used by | Flag to override the path |
|------|---------|---------------------------|
| `ffmpeg` | every encoding subcommand | `--ffmpeg` (`compare`, `ladder`) or `--ffmpeg-bin` (all others) |
| `vmaf` (libvmaf CLI) | every scoring subcommand | `--vmaf` (`compare`, `ladder`) or `--vmaf-bin` (all others) |
| `ffprobe` | `corpus` (HDR detection), `predict`, `auto`, `tune-per-shot` | `--ffprobe-bin` (`corpus`, `predict`) |
| `vmaf-perShot` | `predict`, `tune-per-shot` | `--per-shot-bin` |
| `vmafx-ort-runner` | `--model` on `predict`, `sidecar`, `auto` | on `PATH` (see [ONNX predictor models](#onnx-predictor-models)) |

## Subcommands at a glance

| Subcommand | What it does | Needs |
|------------|--------------|-------|
| [`compare`](#compare-rate-quality-sweep) | Bisects a CRF per (codec, VMAF target) and ranks the results | a reference video |
| [`ladder`](#ladder-per-title-abr-ladder) | Builds a per-title ABR bitrate ladder from the convex hull | a reference video |
| [`report`](#report-render-markdown-or-html) | Renders Markdown or HTML from `compare` / `ladder` JSON | prior JSON output |
| [`recommend`](#recommend-pick-the-crf-meeting-a-target) | Picks the CRF meeting a VMAF or bitrate target | a source, or an existing corpus |
| [`predict`](#predict-predict-per-shot-vmaf-then-verify) | Predicts per-shot VMAF, verifies on K real encodes | a source |
| [`recommend-saliency`](#recommend-saliency-saliency-aware-roi-encode) | One encode biased toward salient regions | a raw YUV |
| [`prefilter`](#prefilter-joint-deband-and-crf-autotune) | Joint TPE autotune of deband strengths and CRF | a raw YUV, or `--smoke` |
| [`fast`](#fast-proxy-tpe-and-gpu-verify-recommend) | Proxy + TPE + real verify, without the full grid | a raw YUV, or `--smoke` |
| [`auto`](#auto-phase-f-adaptive-planner) | Recipe-aware planner that emits (and can run) a plan | a source |
| [`corpus`](#corpus-phase-a-grid-sweep) | Sweeps a (preset, CRF) grid into a JSONL corpus | sources, geometry, presets |
| [`benchmark`](#benchmark-rank-encoders-from-a-corpus) | Ranks encoders from an existing corpus | a corpus JSONL |
| [`encode-profile`](#encode-profile-reproduce-one-recommendation) | Re-encodes one recommendation from a report | a report with `encoder_profile` |
| [`tune-per-shot`](#tune-per-shot-per-shot-crf-tuning) | Detects shots, bisects a CRF per shot, emits an FFmpeg plan | a source |
| [`sidecar`](#sidecar-local-predictor-sidecar) | Trains and inspects an on-host predictor correction | feature JSON |

A typical first run needs only a reference clip and two binaries on `PATH`:

```bash
vmafx-tune-go compare --reference src.mp4 --targets 85,90 --output results.json
vmafx-tune-go report results.json --format html --output report.html
```

## compare: rate-quality sweep

`compare` runs a VMAF-target bisect for each `(codec, target)` pair and emits a
ranked report. Stage 1 supports software encoders only: `libx264` and
`libx265`.

The `vmaf` binary reads only Y4M (or raw YUV with explicit geometry), so the
scorer decodes the reference to Y4M once per run, unless it already is a
`.y4m` file, and decodes each Matroska encode to Y4M as it is scored. The
reference can therefore be a `.y4m` file or any container FFmpeg reads; a
headerless `.yuv` reference is refused, because the scorer has no geometry for
it. The decoded files are written to `--work-dir` and removed after use.

```text
vmafx-tune-go compare --reference <video> [flags]
```

| Flag | Default | Description |
|------|---------|-------------|
| `--reference`, `-r` | — | **Required.** Path to the reference video file. |
| `--codecs`, `-c` | `libx264,libx265` | Comma-separated encoder names (`libx264`, `libx265`). |
| `--targets`, `-t` | `85` | Comma-separated VMAF target(s). Several targets produce a schema-v2 sweep. |
| `--output`, `-o` | stdout | Output file path. |
| `--format` | `json` | `json` or `markdown`. |
| `--ffmpeg` | `ffmpeg` | Path to the ffmpeg binary. |
| `--vmaf` | `vmaf` | Path to the vmaf binary used for scoring. |
| `--work-dir` | OS temp dir | Directory for temporary encode outputs. |
| `--crf-lo` | `0` | Lower bound of the CRF search window (best quality). |
| `--crf-hi` | `0` | Upper bound; `0` means the encoder default (51 for x264 and x265). |
| `--max-iter` | `12` | Maximum bisect iterations per (codec, target) pair. |

Single target, JSON output:

```bash
vmafx-tune-go compare \
  --reference src.mp4 \
  --codecs libx264,libx265 \
  --targets 85 \
  --output results.json
```

Multi-target sweep, Markdown output:

```bash
vmafx-tune-go compare \
  --reference src.mp4 \
  --codecs libx264 \
  --targets 80,85,90,95 \
  --format markdown
```

### compare output schema

A single `--targets` value produces schema-v1 JSON, identical to the Python
`vmaf-tune compare` JSON output:

```json
{
  "src": "src.mp4",
  "target_vmaf": 85.0,
  "tool_version": "dev",
  "wall_time_ms": 4200,
  "rows": [
    {
      "codec": "libx264",
      "adapter": "...",
      "runtime_variant": "...",
      "ffmpeg_bin": "ffmpeg",
      "encoder_version": "...",
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

Several `--targets` values produce schema-v2 output (adds `schema_version: 2`
and `target_vmafs: [...]`), also compatible with the Python `report.py`
renderer. Non-finite float values (`NaN`, `Inf`) are written as `null`, so the
output is RFC 8259 strict JSON that Go, Rust and `jq --strict` all parse.

## ladder: per-title ABR ladder

`ladder` builds an ABR bitrate ladder for one source. For each `(resolution,
VMAF target)` cell it bisects the highest CRF that still meets the target. The
resulting (bitrate, VMAF) cloud is reduced to its upper convex hull (the Pareto
frontier), and a small set of knee renditions is selected from the hull.

Every cell runs at its own resolution, as the Python `vmaf-tune ladder` does:
each probe encode scales the source with `-vf scale=W:H`, and the reference is
decoded to Y4M through the same filter once per cell, so a 640x360 rung is
encoded at 640x360 and scored against a 640x360 reference. When no cell
produces a scored encode, `ladder` exits `2` with the first cell's error and
writes no ladder.

Each cell is also scored with the VMAF model its own height selects, the rule
of [resolution-aware model selection](vmaf-tune-resolution-aware.md): a rung
2160 lines or taller uses `vmaf_v1.0.16_1d5h_2160`, every lower rung
`vmaf_v1.0.16_3d0h`, passed to libvmaf as `--model version=...`. A ladder that
mixes 1080p and 2160p rungs therefore scores them with different models, as
the Python `ladder` does. The Go ladder has no `--vmaf-model` or `--neg` flag
yet.

```text
vmafx-tune-go ladder --reference <video> [flags]
```

| Flag | Default | Description |
|------|---------|-------------|
| `--reference`, `-r` | — | **Required.** Path to the reference video. |
| `--codec`, `-c` | `libx264` | Encoder name. Ten encoders are accepted: `libx264`, `libx265`, `libsvtav1`, `libaom-av1`, `h264_nvenc`, `hevc_nvenc`, `h264_qsv`, `hevc_qsv`, `h264_amf`, `hevc_amf`. |
| `--resolutions` | `320x240,480x360,640x480,768x432,1280x720,1920x1080` | Comma-separated `WxH` grid. |
| `--targets`, `-t` | `75,85,95` | Comma-separated VMAF targets for the sampling grid, each in (0, 100]. |
| `--output`, `-o` | stdout | Output file path. |
| `--format` | `json` | `json` or `markdown`. |
| `--ffmpeg` / `--vmaf` | `ffmpeg` / `vmaf` | Binary paths. |
| `--work-dir` | OS temp dir | Directory for temporary encode outputs. |
| `--crf-lo` / `--crf-hi` | `0` / `0` | CRF search window; `0` means the encoder default. |
| `--max-iter` | `12` | Maximum bisect iterations per (resolution, target) cell. |
| `--max-rungs` | `6` | Maximum renditions selected from the hull. |
| `--min-bitrate-gap` | `100` | Minimum bitrate gap between adjacent renditions, in kbps. |

```bash
vmafx-tune-go ladder \
  --reference src.mp4 \
  --codec libx264 \
  --targets 75,85,95 \
  --output ladder.json
```

The JSON is a superset of the Python `vmaf-tune ladder` schema (schema version
1: `src`, `encoder`, `target_vmafs`, `resolutions`, `tool_version`,
`wall_time_ms`, `cloud`, `hull`, `renditions`) and works with the existing
HLS/DASH manifest renderer. Failed points carry `ok: false`, and non-finite
bitrate or VMAF values are written as `0` rather than `null`.

## report: render Markdown or HTML

`report` reads one or more JSON files produced by `compare` or `ladder` and
renders a human-readable report. It auto-detects whether each input is a
`compare` or a `ladder` file (it probes the top-level `rows` and `renditions`
keys) and merges them into one report, sorted by `tool_version` and `src`.

```text
vmafx-tune-go report <input.json> [input2.json ...] [flags]
```

| Flag | Default | Description |
|------|---------|-------------|
| `--output`, `-o` | stdout | Output file path. |
| `--format` | `markdown` | `markdown` or `html`. |

At least one input file is required.

```bash
vmafx-tune-go report results.json
```

Merge `compare` and `ladder` results into one HTML report:

```bash
vmafx-tune-go report compare.json ladder.json \
  --format html \
  --output report.html
```

The HTML output is self-contained (inlined CSS, no external dependencies) and
renders without a network connection. PDF and JSON-diff modes are not
implemented.

## recommend: pick the CRF meeting a target

`recommend` has two modes. With `--from-corpus` it picks from an existing
corpus JSONL and runs no new encodes. Without it, a coarse-to-fine CRF search
runs the encodes first, writes every visited point to `--output`, and picks
from those rows.

```text
vmafx-tune-go recommend [flags]
```

The ML-driven group (`recommend`, `predict`, `recommend-saliency`, `prefilter`)
emits JSON that is byte-identical to the Python originals: key order, the
`", "` and `": "` separators, `NaN` / `Infinity` tokens and float rendering all
match `json.dumps`. A downstream consumer cannot tell which binary produced a
file.

### recommend predicates

The two predicates are mutually exclusive.

| Flag | Description |
|------|-------------|
| `--target-vmaf` | Lowest-bitrate row whose VMAF meets the target (ties go to the higher VMAF, then the lower CRF). Falls back to the closest miss, tagged `(UNMET)`, when nothing clears it. |
| `--target-bitrate` | Row whose `bitrate_kbps` is closest to the target; ties go to the lower CRF. `--from-corpus` only. |

### recommend source and encode flags

`--source` and `--preset` are required unless `--from-corpus` is used.

| Flag | Default | Description |
|------|---------|-------------|
| `--from-corpus` | — | Pick from this corpus JSONL instead of running encodes. |
| `--json` | off | Emit the recommendation as one JSON object on stdout. |
| `--source` | — | Reference video; repeatable to sweep several sources. |
| `--width` / `--height` | — | Raw-YUV reference geometry. |
| `--preset` | — | Encoder preset; repeatable to sweep several presets. |
| `--encoder` | `libx264` | Codec adapter (any of the 19 registered adapters). |
| `--pix-fmt` | `yuv420p` | ffmpeg pix_fmt. |
| `--framerate` | `24` | Reference framerate. |
| `--duration` | `0` | Clip duration in seconds; bounds the encode and derives achieved kbps. |
| `--output` | `corpus.jsonl` | JSONL destination for the visited points. |
| `--encode-dir` | `.workingdir/cache/vmafx-tune/encodes` | Bounded scratch directory for the probe encodes. |
| `--keep-encodes` | off | Keep the encoded artefacts instead of deleting them after scoring. |
| `--no-source-hash` | off | Skip the source SHA-256 (faster on very large sources). |
| `--vmaf-model` | `vmaf_v1.0.16_3d0h` | libvmaf model version, or a `path=...` string. |
| `--score-backend` | `auto` | libvmaf backend: `auto`, `cpu`, `cuda`, `sycl`, `hip`, `metal`. |
| `--ffmpeg-bin` / `--vmaf-bin` | `ffmpeg` / `vmaf` | Binary paths. |

### recommend search flags

| Flag | Default | Description |
|------|---------|-------------|
| `--coarse-to-fine` | off | Accepted for parity with Python; the encode-driven mode is always coarse-to-fine. |
| `--coarse-step` | `10` | CRF step for the coarse pass (10 gives 10, 20, 30, 40, 50). |
| `--fine-radius` | `5` | Radius around the best-coarse CRF for the fine pass. |
| `--fine-step` | `1` | CRF step for the fine pass. |

With the defaults that is 5 coarse plus up to 10 fine encodes, against 52 for
a full sweep (ADR-0296). The CRF window is fixed at 10–50, matching the Python
defaults, so a Go and a Python run over the same source visit the same cells
and their corpora stay comparable. See
[vmaf-tune-coarse-to-fine.md](vmaf-tune-coarse-to-fine.md) and
[vmaf-tune-recommend.md](vmaf-tune-recommend.md).

### recommend uncertainty flags

These flags implement ADR-0393.

| Flag | Default | Description |
|------|---------|-------------|
| `--with-uncertainty` | off | Consume the conformal prediction intervals carried in each row's `vmaf_interval` block. |
| `--uncertainty-sidecar` | — | Calibration sidecar JSON. Without one, the documented Research-0067 floor applies (tight 2.0, wide 5.0 VMAF). |

A tight interval whose lower bound already clears the target short-circuits the
search at that row. A wide interval refuses to short-circuit and tags the
result `(UNCERTAIN)`. This changes which encodes get probed, never which get
shipped; the production-flip gate stays in the predictor's validation harness.

### recommend examples

Pick from an existing corpus, machine-readable:

```bash
vmafx-tune-go recommend --from-corpus corpus.jsonl --target-vmaf 93 --json
```

Run the search, then pick:

```bash
vmafx-tune-go recommend \
  --source src.yuv --width 1920 --height 1080 --duration 10 \
  --preset medium --target-vmaf 93 --output corpus.jsonl
```

## predict: predict per-shot VMAF, then verify

`predict` predicts VMAF per shot from cheap signals, then verifies the
prediction against real libvmaf on K stratified shots and emits a verdict.

```text
vmafx-tune-go predict --source <video> [flags]
```

| Flag | Default | Description |
|------|---------|-------------|
| `--source` | — | **Required.** Reference video, any FFmpeg-readable container. |
| `--codec` | `libx264` | Codec adapter. |
| `--target-vmaf` | `93` | Target pooled-mean VMAF. |
| `--validate-k` | `8` | Shots to verify against real libvmaf. |
| `--residual-threshold` | `1.5` | Max `abs(predicted - measured)` before the verdict leaves `gospel`. |
| `--model` | — | `predictor_<codec>.onnx`; without it the per-codec analytical curve runs. |
| `--use-saliency` | off | Include saliency mean and variance in the feature vector. |
| `--saliency-model` | shipped model | `saliency_student_v1.onnx` path. |
| `--per-shot-bin` | `vmaf-perShot` | Shot detector binary. |
| `--ffmpeg-bin` / `--ffprobe-bin` / `--vmaf-bin` | `ffmpeg` / `ffprobe` / `vmaf` | Binary paths. |
| `--bitdepth` | `8` | Source bit depth (8, 10 or 12), forwarded to the detector. |
| `--total-frames` | `0` | Frame count for the single-shot fallback. |
| `--report-out` | stdout | Validation report destination. |
| `--with-uncertainty` | off | Emit conformal intervals beside each predicted VMAF. |
| `--calibration-sidecar` | — | Split-conformal calibration JSON. |
| `--alpha` | sidecar's | Override the miscoverage level (0.05 gives 95 % coverage). |

### predict verdicts

| Verdict | Meaning | Exit |
|---------|---------|------|
| `gospel` | Every residual within the threshold; trust the predictor on the remaining shots. | 0 |
| `recalibrate` | Residuals biased but tight; add the reported `bias_correction` and redo the picks. No retraining needed. | 0 |
| `fall_back` | Residuals too wide; degrade to the full encode-and-score loop. | 2 |

Shot detection degrades to a single shot spanning the clip when `vmaf-perShot`
is unavailable, with a `WARN` log line.

!!! warning
    Without `--calibration-sidecar`, `--with-uncertainty` emits degenerate
    `low == high == point` intervals and flags the report `"calibrated": false`.
    Do not read a coverage guarantee into a zero-width interval.

```bash
vmafx-tune-go predict --source movie.mkv --codec libx264 \
  --target-vmaf 93 --validate-k 8 --report-out predict.json
```

## recommend-saliency: saliency-aware ROI encode

`recommend-saliency` scores the fork's `saliency_student_v1` model over sampled
frames, reduces the result to a per-block QP-offset map, and hands it to the
encoder through its native ROI channel.

```text
vmafx-tune-go recommend-saliency --src <yuv> --width W --height H \
  --duration-frames N --output <path> [flags]
```

`--src`, `--width`, `--height`, `--duration-frames` and `--output` are
required.

| Flag | Default | Description |
|------|---------|-------------|
| `--src` | — | Raw YUV reference. |
| `--width` / `--height` | — | Reference geometry. Any size works: the model sees the frame zero-padded to a multiple of 32 and the map is cropped back. |
| `--pix-fmt` | `yuv420p` | ffmpeg pix_fmt. |
| `--framerate` | `24` | Reference framerate. |
| `--duration-frames` | — | Frame count to score saliency over, typically the full clip. |
| `--output` | — | Encode destination (mp4, mkv, ...). A `.json` path is a report destination: the encode goes to a sibling `<stem>_encoded.mp4` and the path is echoed on stdout. |
| `--saliency-aware` | off | Enable the biasing; without it this is a plain encode. |
| `--saliency-offset` | `-4` | QP delta at peak saliency, clamped to ±12. Negative spends more bits on salient regions. |
| `--saliency-aggregator` | `mean` | Temporal reducer: `mean`, `ema`, `max` or `motion-weighted`. |
| `--saliency-ema-alpha` | `0.6` | Current-frame weight for `ema`. |
| `--saliency-model` | shipped model | ONNX path. |
| `--saliency-fallback-plain` | off | Accept a plain encode on an encoder with no ROI dispatch instead of exiting 2 (ADR-0546). |
| `--encoder` | `libx264` | Codec adapter. |
| `--crf` | adapter default | Explicit CRF. |
| `--preset` | `medium` | Encoder preset. |
| `--ffmpeg-bin` | `ffmpeg` | ffmpeg binary. |

### ROI channel per encoder

| Encoder | Channel | Granularity |
|---------|---------|-------------|
| `libx264` | `-qpfile …` (patched FFmpeg bridge, x264 `quant_offsets`) | 16×16 macroblocks |
| `libaom-av1` | `-qpfile …` (patched FFmpeg bridge) | 16×16 macroblocks |
| `libx265` | `-x265-params zones=…` | per-clip spatial mean |
| `libsvtav1` | `-svtav1-params qp-file=…` | 64×64 super-blocks |
| `libvvenc` | `-vvenc-params ROIFile=…` | 64×64 CTUs |

Any other encoder exits 2 unless `--saliency-fallback-plain` is set or
`VMAFTUNE_SALIENCY_FALLBACK_OK=1` is exported. See also
[vmaf-tune-saliency-aware.md](vmaf-tune-saliency-aware.md).

!!! note
    The Go port ships the entire numeric pipeline (YUV to ImageNet tensor, all
    four temporal aggregators, the QP mapping, the per-block reduce and every
    sidecar format) but has no in-process ONNX Runtime. With `--saliency-aware`
    it logs a warning and falls back to a plain encode, the same degradation
    the Python takes when `onnxruntime` is not installed. See
    [Saliency ONNX inference](#saliency-onnx-inference).

## prefilter: joint deband and CRF autotune

`prefilter` optimises the ten frozen Pelorus deband knobs (the ADR-0110
control-plane contract) and the CRF axis together in one TPE study, with VMAF
as the oracle.

```text
vmafx-tune-go prefilter --target-vmaf T [flags]
```

| Flag | Default | Description |
|------|---------|-------------|
| `--target-vmaf` | — | **Required.** Quality target on the VMAF [0, 100] scale. |
| `--smoke` | off | Use the synthetic surface; no ffmpeg, Vulkan or GPU. |
| `--src` | — | Source video; required for the live loop. |
| `--width` / `--height` | — | Raw-YUV geometry; required for the live loop. |
| `--pix-fmt` | `yuv420p` | ffmpeg pix_fmt. |
| `--framerate` | `24` | Reference framerate. |
| `--duration` | `0` | Clip duration in seconds. |
| `--sweep-knob` | all ten | Restrict the search to this knob; repeatable. |
| `--crf-min` / `--crf-max` | `18` / `40` | Joint search range. |
| `--n-trials` | `60` live, `40` smoke | TPE trial budget. |
| `--time-budget-s` | `600` | Soft wall-clock cap. |
| `--seed` | `0` | Sampler seed; the same seed reproduces the same recommendation. |
| `--encoder` | `libx264` | Codec performing the post-deband encode. |
| `--preset` | `medium` | Encoder preset. |
| `--filter` | `pelorus_deband` | Filter adapter to autotune. |
| `--score-backend` | `auto` | libvmaf backend. |
| `--ffmpeg-bin` / `--vmaf-bin` | `ffmpeg` / `vmaf` | Binary paths. |
| `--vmaf-model` | `vmaf_v1.0.16_3d0h` | libvmaf model version. |
| `--encode-dir` | `.workingdir/cache/vmafx-tune/prefilter` | Probe scratch directory. |
| `--output` | stdout | JSON destination. |

The objective is `|achieved - target| + λ·kbps`, so the search converges on the
lowest-bitrate combination that hits the target. The bitrate weight is small
enough that it only breaks ties between equally good quality points.

vmafx never runs the deband filter itself; it only emits the `-vf` string. The
live loop therefore requires the `pelorus_deband_vulkan` filter in the ffmpeg
build and refuses to start (exit 2) with an actionable message when it is
absent. This is an FFmpeg filter, unrelated to the removed libvmaf Vulkan
scoring backend (ADR-0726). `--smoke` exercises the whole search without it.

The ten swept knobs are `range`, `thry`, `thrc`, `grainy`, `grainc`,
`softness`, `detail`, `dither`, `dynamic` and `protect`. The deliberately
out-of-contract options (`sample`, `blur`, `planes`, `meta`) are pipeline
switches set once per run and are rejected if passed to `--sweep-knob`.

```bash
# CI-friendly: no ffmpeg, no Vulkan filter, no GPU.
vmafx-tune-go prefilter --smoke --target-vmaf 93 --output rec.json

# Live loop.
vmafx-tune-go prefilter \
  --src ref.yuv --width 1920 --height 1080 --duration 10 \
  --target-vmaf 93 --encoder libx264 --output rec.json
```

Unlike the Python subcommand, this needs no optional `[fast]` extra: the TPE
sampler is implemented natively (see [TPE sampler
trajectory](#tpe-sampler-trajectory)).

## fast: proxy, TPE and GPU-verify recommend

`fast` recommends a CRF for a VMAF target without running the full Phase A
grid. A TPE (Tree-structured Parzen Estimator) search walks the integer CRF
axis. Each trial encodes a short probe slice, extracts the canonical-6 libvmaf
features, and predicts VMAF with the `fr_regressor_v2` proxy. One real encode
plus libvmaf score at the chosen CRF then verifies the recommendation: the
proxy alone never wins (ADR-0304). The pick is the
lowest predicted bitrate among the CRFs that meet the target, as in
`recommend`. See also
[vmaf-tune-fast-path.md](vmaf-tune-fast-path.md).

```text
vmafx-tune-go fast --target-vmaf <N> [--smoke | --src <file> --width W --height H] [flags]
```

!!! warning "Port status"
    `--smoke` runs end to end today. Production mode runs the backend
    selection, the probe encodes, the canonical-6 extraction and the verify
    pass, but stops at the proxy-inference step: `fr_regressor_v2` is a
    two-named-input ONNX graph and the Go inference seam drives a single flat
    input vector only. See [Production-mode
    blocker](#production-mode-blocker-onnx-named-inputs)
    and use `vmaf-tune fast` for a production run in the meantime.

Smoke run (works on any host):

```bash
vmafx-tune-go fast --smoke --target-vmaf 90
```

### fast flags

| Flag | Default | Description |
|------|---------|-------------|
| `--target-vmaf` | — | **Required.** Quality target on the VMAF [0, 100] scale. |
| `--src` | — | Source video (raw YUV or any ffmpeg-readable container). Required unless `--smoke`. |
| `--width` / `--height` | `0` | Raw-YUV reference geometry. Required in production mode. |
| `--pix-fmt` | `yuv420p` | ffmpeg pixel format. |
| `--framerate` | `24` | Reference framerate. |
| `--encoder` | `libx264` | Encoder. In production mode it must be one of the ten Go encoders and in the proxy model's encoder vocabulary. |
| `--preset` | `medium` | Encoder preset for the probe and verify encodes. |
| `--crf-min` / `--crf-max` | `10` / `51` | Inclusive CRF search range. |
| `--n-trials` | `30` prod / `50` smoke | TPE trial budget. |
| `--time-budget-s` | `300` | Soft wall-clock cap on the TPE loop. In-flight trials finish. |
| `--proxy-tolerance` | `1.5` | Max absolute proxy/verify VMAF gap before the result is flagged out-of-distribution. |
| `--sample-chunk-seconds` | `5.0` | Probe-encode slice length per trial. Shorter is faster, longer gives more stable features. |
| `--smoke` | `false` | Deterministic synthetic CRF→VMAF curve. No ffmpeg, no ONNX, no GPU verify. |
| `--score-backend` | `auto` | libvmaf backend for the verify pass: `auto`, `cpu`, `cuda`, `sycl`, `hip`, `metal`. `auto` walks cuda → sycl → hip → metal → cpu over the backends `vmaf --list-backends` reports usable; an explicit value is honoured strictly and errors rather than downgrading. |
| `--ffmpeg-bin` | `ffmpeg` | Path to the ffmpeg binary. |
| `--vmaf-bin` | `vmaf` | Path to the libvmaf CLI binary. |
| `--vmaf-model` | `vmaf_v1.0.16_3d0h` | vmaf model version string. |
| `--encode-dir` | `.workingdir/cache/vmafx-tune/fast` | Scratch dir for probe and verify encodes. |
| `--output`, `-o` | stdout | JSON destination for the recommendation payload. |

### fast exit codes

These are identical to `vmaf-tune fast`.

| Code | Meaning |
|------|---------|
| `0` | Recommendation emitted; proxy and verify agree within `--proxy-tolerance`. |
| `2` | Usage or environment error (bad CRF range, missing `--src`, unavailable backend, proxy unavailable). |
| `3` | Recommendation emitted, but the proxy/verify gap exceeds tolerance. Fall back to the slow Phase A grid (ADR-0276). The payload is still written. |

### Production-mode blocker: ONNX named inputs

The shipped proxy `model/tiny/fr_regressor_v2.onnx` declares two named input
ports: `features` (shape `[N, 6]`) and `codec` (shape `[N, 14]`), as recorded
in its sidecar's `"input_names"`. The only ONNX inference seam in the Go tree,
`pkg/ai.Registry.Infer`, serialises a single flat `[]float64` to the
`vmafx-ort-runner` subprocess and has no wire format for a second port.
`Registry.InferDirect`, the CGO path, is an explicit Stage-2 stub.

Flattening the two ports into one 20-D vector is not a workaround.
`vmaftune/proxy.py` documents that exact mistake: the graph's first dense layer
reads the 6-D `features` port only, so the 14 codec dimensions are silently
interpreted as batch padding and `codec` receives nothing. Rather than return a
quietly wrong score, `pkg/fast` fails with `ErrProxyPortsUnsupported` and a
diagnostic naming both ports.

Any one of these unblocks it:

1. A `vmafx-ort-runner` protocol that accepts named input tensors, plus a
   matching `pkg/ai.Registry.InferNamed`. The runner is in-tree since
   ADR-1134 ([vmafx-ort-runner.md](vmafx-ort-runner.md)), so this is a
   protocol extension rather than an external dependency.
2. Promoting `pkg/ai.Registry.InferDirect` onto a CGO ONNX Runtime binding
   (for example `github.com/yalue/onnxruntime_go`), which `pkg/ai` defers to
   Stage 2 because it couples the build to `libonnxruntime`.
3. A single-port re-export of `fr_regressor_v2` that concatenates the two
   inputs inside the graph, shipped alongside the current model.

### Parity with the Python fast implementation

The Go port found four defects in `vmaf-tune fast`; the Python path was fixed
to match on 2026-09-04 (`T-VMAFTUNE-FAST-PY-PROBE-BROKEN-2026-08-30` in
[the bug ledger](../state.md)). Both implementations now:

1. decode container-shaped probe and verify encodes to raw YUV before the
   libvmaf CLI scores them;
2. read the `integer_`-prefixed canonical-6 keys first, then the bare key, then
   a per-frame average;
3. place the codec one-hot by the model's vocabulary (`libvvenc` at index 3,
   `unknown` at index 11), mapping a codec outside it to `unknown`. Go reads
   the vocabulary from the model sidecar's `encoder_vocab`; Python keeps
   `proxy.ENCODER_VOCAB_V2` aligned with that sidecar and names the
   substitution on stderr and as `proxy_encoder_slot` in its JSON;
4. standardise the features with the sidecar's `feature_mean` / `feature_std`
   before inference.

`tools/vmaf-tune/tests/test_fast_parity.py::test_e2e_probe_extraction_parity`
runs both on the same clip and holds the raw and normalised features to 1e-6.

One behaviour is weaker in Go than in Python: TPE reproducibility. Optuna's
`TPESampler(seed=0)` makes a run bit-reproducible. The Go port uses
`github.com/c-bata/goptuna`, whose TPE sampler honours its seed only partially:

- `tpe.SamplerOptionSeed` seeds the sampler's own RNG and its startup random
  sampler.
- `goptuna/internal/random.ArgMaxMultinomial` draws from the process-global
  `math/rand` source, which Go seeds randomly at startup and which `rand.Seed`
  can no longer override.

Repeat runs therefore explore slightly different trial sequences and may
return a neighbouring CRF when two candidates score within about a VMAF point
of each other. Measured on the ADR-0276 smoke curve: at the shipped budgets the
recommendation stays within ±1 of the brute-force optimum in about 99–100 % of
runs, and at 150 trials it hit the exact optimum in 150 of 150 runs for every
target tested. Closing the gap needs an upstream goptuna change threading the
sampler RNG into `internal/random`.

## auto: Phase F adaptive planner

`auto` composes the per-phase tuning stages into one deterministic decision
tree and emits a JSON plan. Optionally it realises the winning cell as a real
encode plus a libvmaf score.

```text
vmafx-tune-go auto --src <video> [flags]
```

`--src` is required unless `--smoke` is given: the smoke planner probes
nothing, and the plan then records an empty `src`. `--execute` always needs
`--src`. A missing `--src` exits `2`.

| Flag | Default | Description |
|------|---------|-------------|
| `--src` | — | Source video. **Required** unless `--smoke` (and always with `--execute`). |
| `--target-vmaf` | `93` | Target pooled-mean VMAF on the standard `[0, 100]` scale. |
| `--max-budget-bitrate` | `8000` | Upper bound on the picked rendition's bitrate, in kbps. |
| `--allow-codecs` | `libx264` | Comma-separated codec list the tree may pick from. A single entry short-circuits the compare-shortlist stage. |
| `--codec` | unset | Pin the codec choice, overriding the `--allow-codecs` ranking. Also short-circuits the shortlist stage. |
| `--sample-clip-seconds` | `0` | Propagate this clip length to internal sweeps rather than re-deciding per stage. `0` means the full source. |
| `--smoke` | `false` | Exercise the composition with synthetic metadata: no ffprobe, no ffmpeg, no ONNX. |
| `--output` | stdout | Write the JSON plan here. |
| `--execute` | `false` | After planning, run real FFmpeg encodes and libvmaf scores for the selected cell(s). |
| `--runs-dir` | `runs` | Output directory for encoded files and `tune_results.jsonl` (used with `--execute`). |
| `--execute-all` | `false` | With `--execute`: run every plan cell, not just the winner. Useful for post-hoc A/B comparison. |
| `--model` | unset | Optional `predictor_<codec>.onnx` path. The default uses the analytical fallback curve. |

`--model` is a Go-side addition. The Python `auto` driver always constructs its
predictor without a model path, which is the analytical fallback this flag
defaults to. Supplying a model routes inference through the ONNX bridge in
`pkg/ai`, which degrades back to the analytical curve (with a warning) when the
ORT runner is not on `PATH`.

Plan only:

```bash
vmafx-tune-go auto \
  --src src.mp4 \
  --target-vmaf 93 \
  --allow-codecs libx264,libx265
```

Plan and realise the winner:

```bash
vmafx-tune-go auto \
  --src src.mp4 \
  --target-vmaf 95 \
  --max-budget-bitrate 6000 \
  --execute --runs-dir runs/
```

### How the plan is built

1. **Probe** the source: geometry and duration via `ffprobe`, HDR signalling
   via the colour-metadata classifier. Every probe failure degrades to
   conservative defaults (1920x1080, duration 0, SDR) rather than aborting the
   run.
2. **Apply the content recipe.** The content class selects a small override set:
   a narrower or wider conformal-confidence gate, a forced single-rung ladder,
   a saliency intensity, and a target-VMAF offset. An HDR source carrying only
   a generic `live_action` label is promoted to the HDR recipe. The overrides
   load from `ai/data/phase_f_recipes_calibrated.json` when that file is
   reachable, and fall back to the documented placeholders with a one-line
   warning otherwise.
3. **Walk the ten short-circuits**, recording each one that fires under
   `metadata.short_circuits`.
4. **Estimate each `(rung, codec)` cell**: invert the predictor for a CRF, then
   estimate the VMAF and bitrate that CRF would produce.
5. **Pick a winner** against the VMAF target and the bitrate budget.

### Short-circuits

Each predicate names a stage the tree can skip. They are evaluated in this
order, and the order is part of the output contract.

| Name | Fires when |
|------|-----------|
| `ladder-single-rung` | Source height is below 2160, or the recipe forces a single rung. |
| `codec-pinned` | `--codec` is set, or `--allow-codecs` resolves to one entry. |
| `predictor-gospel` | The predictor verdict is `GOSPEL`; trust its CRF and skip the coarse-to-fine fallback. |
| `skip-saliency` | Content class is neither `animation` nor `screen_content`. |
| `sdr-skip` | The source carries no HDR signalling. |
| `sample-clip-propagate` | `--sample-clip-seconds` is positive; propagate it verbatim to internal sweeps. |
| `skip-per-shot` | The source is both shorter than 5 minutes and below 0.15 shot variance. |
| `low-complexity` | The probe-encode bitrate is under 200 kbps. Dormant when no probe has run. |
| `baseline-meets-target` | A default-CRF encode already meets the target. Dormant when no baseline was scored. |
| `no-two-pass` | The resolved codec adapter does not support two-pass encoding. |

### Confidence-aware escalation

Each cell carries a conformal interval width, and that width decides whether
the predictor's own verdict is overridden.

| Interval width | Decision |
|----------------|----------|
| `<= tight` (default 2.0) | `skip-escalation`: trust the point estimate even on a `FALL_BACK` verdict. |
| `>= wide` (default 5.0) | `force-escalation`: escalate even on a `GOSPEL` verdict. |
| between | Defer to the native verdict. |
| `NaN` (uncalibrated) | Defer to the native verdict. |

Without a calibration sidecar the interval is uncalibrated, so cells carry
`NaN` and no override happens. The 2.0 / 5.0 defaults are an emergency floor,
not a corpus fit.

### Plan JSON

The plan is a `{"cells": [...], "metadata": {...}}` object with sorted keys,
byte-compatible with the Python `vmaf-tune auto` output. Each cell carries
`rung`, `codec`, `verdict`, `crf`, `estimated_vmaf`,
`estimated_bitrate_kbps`, `hdr_args`, `sample_clip_seconds`,
`confidence_decision`, `interval_width`, `effective_predictor_target_vmaf`,
`prediction_source`, `saliency_intensity` and `selected`.

`metadata.winner.status` is one of:

| Status | Meaning |
|--------|---------|
| `budget_and_quality_met` | A cell satisfies both the target and the budget. |
| `quality_met_budget_exceeded` | Quality is reachable, but every such cell is over budget; the smallest overage wins. |
| `target_unmet` | No cell reaches the target; the closest miss is returned so you get a concrete next encode. |
| `no_eligible_cells` | No cell carried finite estimates. |

!!! warning
    The plan JSON is not strict RFC 8259. An uncalibrated `interval_width` is
    emitted as the bare token `NaN`, exactly as CPython's `json.dumps` does
    with its default `allow_nan=True`. This is deliberate byte-compatibility
    with the Python emitter. Parse it with Python's `json` module or another
    permissive parser; `jq --strict` and Go's `encoding/json` reject it. The
    `--execute` results log (`tune_results.jsonl`) is strict: non-finite
    values there are rendered as `null`.

### Execute mode

With `--execute` the selected cell is encoded and scored, and one row per
executed cell is appended to `<runs-dir>/tune_results.jsonl`. Encoded files
land beside it as `encode_<NNN>_<codec>_<preset>_crf<n>.mkv` (`NNN` is the
zero-padded cell index).

A failed encode is recorded in its row with a non-zero `encode_exit_status`,
and scoring is skipped for that cell. The command exits non-zero only when
cells were executed and none scored successfully.

## corpus: Phase A grid sweep

`corpus` sweeps a `(preset, crf)` grid against one or more references, encodes
each cell, scores it against the reference with the libvmaf CLI, and writes one
JSONL row per `(source, preset, crf)` combination.

```text
vmafx-tune-go corpus [flags]
```

`--source`, `--width`, `--height` and `--preset` are always required, and
`--crf` is required unless `--coarse-to-fine` derives the axis.

The JSONL schema (v3) is the API contract the Phase B target-VMAF bisect and
the Phase C per-title CRF predictor consume; see
[vmaf-tune-corpus.md](vmaf-tune-corpus.md) for the column reference. The Go
writer emits the same bytes the Python writer does, including the bare `NaN`
tokens CPython's `json` module produces for columns libvmaf did not populate,
so a corpus written by either binary is readable by the same trainers.

When scoring with models that omit VIF (such as the default
`vmaf_v1.0.16_3d0h` model, ADR-1168 / ADR-1169), every Go libvmaf driver
(`corpus`, `fast`, `scorecli`, `tune/executor`) automatically passes
`--feature vif` to libvmaf and parses options-suffixed keys. This keeps the
canonical-6 columns (`adm2`, `vif_scale0..3`, `motion2`) populated.

### corpus source and encode flags

| Flag | Default | Description |
|------|---------|-------------|
| `--source` | — | **Required.** Reference video. Repeat for multiple sources. |
| `--width` | — | **Required.** Rung target width in pixels. |
| `--height` | — | **Required.** Rung target height in pixels. |
| `--preset` | — | **Required.** Encoder preset. Repeat for multiple presets. |
| `--crf` | — | Quality value. Repeat for multiple cells. Required unless `--coarse-to-fine` derives the axis. |
| `--pix-fmt` | `yuv420p` | ffmpeg `pix_fmt` of the reference. |
| `--framerate` | `24` | Reference framerate. |
| `--duration` | `0` | Reference duration in seconds. Bounds the encode and the bitrate calculation; `0` means the full source. |
| `--encoder` | `libx264` | Codec adapter. Any registered adapter is accepted; see [vmaf-tune-codec-adapters.md](vmaf-tune-codec-adapters.md). Single-valued. |
| `--two-pass` | off | Run a 2-pass encode for codecs that support it (libx264 / libx265). Adapters without true 2-pass emit a one-line stderr warning and run single-pass (ADR-0333). |
| `--sample-clip-seconds` | `0` | Encode and score only the centre N-second slice of each source. Encode time scales linearly with the slice; expect a 1–2 VMAF-point delta versus full-clip on diverse content (ADR-0297). |
| `--encode-dir` | `.workingdir/cache/vmafx-tune/encodes` | Bounded scratch directory for encodes. |
| `--keep-encodes` | off | Retain encoded outputs after scoring and record their paths in `encode_path`. |
| `--no-source-hash` | off | Skip `src_sha256`. Faster on huge YUVs; loses provenance. |
| `--output` | `corpus.jsonl` | JSONL output path. |

### corpus scoring flags

| Flag | Default | Description |
|------|---------|-------------|
| `--vmaf-model` | `vmaf_v1.0.16_3d0h` | libvmaf model version string. |
| `--neg` | off | Use the VMAF NEG (No Enhancement Gain) variant. Use for codec A-vs-B comparisons; not for production monitoring (see [vmaf-neg.md](../metrics/vmaf-neg.md)). With the default model it selects `vmaf_v0.6.1neg`, because no NEG counterpart exists for `vmaf_v1.0.16_*`. |
| `--score-backend` | `auto` | libvmaf backend: `auto`, `cpu`, `cuda`, `sycl`, `hip`, `metal`. `auto` picks the first of cuda > sycl > hip > metal > cpu that `vmaf --list-backends` reports usable; a specific name is honoured strictly and errors out when unavailable. See [score backends](vmaf-tune-score-backend.md). |
| `--ffmpeg-bin` | `ffmpeg` | Path to the ffmpeg binary. |
| `--vmaf-bin` | `vmaf` | Path to the vmaf binary. |
| `--ffprobe-bin` | `ffprobe` | Path to the ffprobe binary (used for HDR detection). |

### corpus search-mode flags

| Flag | Default | Description |
|------|---------|-------------|
| `--coarse-to-fine` | off | Run a 2-pass coarse-then-fine CRF search instead of the full grid. With the defaults that is 15 encodes rather than 52; see [vmaf-tune-coarse-to-fine.md](vmaf-tune-coarse-to-fine.md). |
| `--coarse-step` | `10` | CRF step for the coarse pass. |
| `--fine-radius` | `5` | ± radius around the best-coarse CRF for the fine pass. |
| `--fine-step` | `1` | CRF step for the fine pass. |
| `--target-vmaf` | unset | Target VMAF. The search refines around the lowest-bitrate CRF whose score meets it; without a target it refines around the highest-VMAF coarse point. |

### corpus HDR flags

The four flags are mutually exclusive; see
[vmaf-tune-hdr-and-sampling.md](vmaf-tune-hdr-and-sampling.md).

| Flag | Description |
|------|-------------|
| `--auto-hdr` | The default. Probe each source with ffprobe and inject HDR codec args (and the HDR-VMAF model) when PQ / HLG signalling is detected. |
| `--force-sdr` | Treat every source as SDR; skip detection and flag injection. |
| `--force-hdr-pq` | Treat every source as HDR PQ (SMPTE-2084) regardless of the probe. |
| `--force-hdr-hlg` | Treat every source as HDR HLG (ARIB STD-B67) regardless of the probe. |

Full grid:

```bash
vmafx-tune-go corpus \
  --source ref.yuv \
  --width 1920 --height 1080 \
  --framerate 24 --duration 10 \
  --preset medium \
  --crf 20 --crf 26 --crf 32 \
  --output corpus.jsonl
```

Coarse-to-fine against a VMAF target:

```bash
vmafx-tune-go corpus \
  --source ref.yuv \
  --width 1920 --height 1080 \
  --preset medium \
  --coarse-to-fine --target-vmaf 93 \
  --output corpus.jsonl
```

### corpus run behaviour

Rows stream to the output file as each cell completes, so an interrupted sweep
leaves a usable partial corpus. The selected scoring backend is echoed on
`stderr` (`vmafx-tune: scoring backend = cpu`) before the first encode, and the
row count is echoed when the sweep finishes.

When `--keep-encodes` is off, cleanup is part of a successful cell. A missing
temporary encode is harmless (an injected runner may already have removed it),
but any other removal failure stops the sweep instead of silently leaking data.
Likewise, when source hashing is enabled, an unreadable source fails before the
first encode; use `--no-source-hash` only when provenance is deliberately not
required.

## benchmark: rank encoders from a corpus

`benchmark` answers the standard post-sweep question: which encoder hit the
target quality at the lowest bitrate? It reads a Phase-A corpus JSONL written by
`corpus` and launches no ffmpeg and no libvmaf; the corpus stays the source of
truth.

```text
vmafx-tune-go benchmark --from-corpus <JSONL> [flags]
```

For every encoder in the corpus the report picks the lowest-bitrate row whose
measured VMAF clears `--target-vmaf`. An encoder that never clears is reported
with status `unmet` and its closest miss, so a missing encoder build is never
mistaken for a quality result.

| Flag | Default | Description |
|------|---------|-------------|
| `--from-corpus` | — | **Required.** Phase-A corpus JSONL to benchmark. |
| `--target-vmaf` | `92` | Matched-quality threshold each encoder must clear. |
| `--baseline-encoder` | lowest-bitrate encoder that clears | Encoder used for the `bitrate_delta_pct` column. |
| `--format` | `markdown` | Report format: `markdown`, `json` or `csv`. |
| `--output`, `-o` | stdout | Report destination. Parent directories are created. |

Markdown to stdout:

```bash
vmafx-tune-go benchmark --from-corpus corpus.jsonl
```

CSV at a stricter target, with a pinned baseline:

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
JSON output is stable pretty RFC 8259 with sorted keys. Both are byte-identical
to `vmaf-tune benchmark`.

## encode-profile: reproduce one recommendation

Every vmaf-tune report embeds a machine-readable `encoder_profile` payload (the
contract is ADR-0643). `encode-profile` reads that payload, selects one
recommendation, and runs the matching FFmpeg encode.

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
then picks the Nth survivor (zero-based, applied after filtering).

| Flag | Default | Description |
|------|---------|-------------|
| `--profile` | — | **Required.** Report JSON / HTML / Markdown containing `encoder_profile`. |
| `--output`, `-o` | — | **Required.** Encoded output path. |
| `--codec` | all | Restrict selection to one codec. |
| `--target-vmaf` | all | Restrict selection to one target VMAF (matched with a 1e-6 absolute tolerance). |
| `--recommendation-index` | `0` (first) | Zero-based index after the filters are applied. |
| `--dry-run` | off | Print the selected recommendation and the exact FFmpeg argv without encoding. |

Override flags (each falls back to the profile value when omitted):

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

Inspect the selection without encoding:

```bash
vmafx-tune-go encode-profile \
  --profile report.json \
  --output /dev/null \
  --dry-run
```

Reproduce the best x265 row at target 95:

```bash
vmafx-tune-go encode-profile \
  --profile report.html \
  --codec libx265 \
  --target-vmaf 95 \
  --output encoded.mkv
```

### encode-profile output

The result is a single JSON document on `stdout` (sorted keys, two-space
indent). A `--dry-run` emits `ok`, `dry_run`, `profile`, `selected`,
`ffmpeg_argv` and `output`. A real run replaces `dry_run` with the encode
outcome:

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

On a real run the process exit status is FFmpeg's own, so a failed encode
surfaces the encoder's code (for example `254`) rather than a generic `1`. The
JSON payload is still written first, so a wrapper script can read
`exit_status` and `stderr_tail` regardless.

### Hardware-encoder caveats

- A QSV row's argv carries FFmpeg's QSV device chain,
  `-init_hw_device vaapi=va:<node> -init_hw_device qsv=qsv_dev@va
  -filter_hw_device qsv_dev`, before the first `-i`, and
  `format=nv12,hwupload=extra_hw_frames=64` at the end of its `-vf` chain
  (see [ADR-0601](../adr/0601-vmaftune-qsv-amf-hw-init-and-probe-fix.md)),
  exactly as the Python `vmaf-tune` builds every QSV encode. The render node is
  `VMAFTUNE_VAAPI_DEVICE`, else the first Intel render node, else
  `/dev/dri/renderD128`.
- Hardware encoders (NVENC / QSV / AMF) reject sources below roughly 320x240.
  A profile built from a smaller clip fails at the encoder.
- `av1_videotoolbox` is a placeholder: upstream FFmpeg ships no such encoder,
  so the adapter refuses to emit an argv shape it cannot verify
  ([ADR-0339](../adr/0339-av1-videotoolbox-placeholder-adapter.md)).

## tune-per-shot: per-shot CRF tuning

`tune-per-shot` cuts the source into shots, bisects a CRF against the VMAF
target inside each shot, and emits an FFmpeg encoding plan: one command per
segment plus the concat-demuxer command that stitches them into the final file.

The plan is emitted, not executed. Segment encodes are independent, so you can
run them sequentially or in parallel, then run the concat command.

```text
vmafx-tune-go tune-per-shot --src <video> [flags]
```

Pipeline:

1. **Shot detection.** Shells out to the fork's `vmaf-perShot` binary
   ([vmaf-perShot.md](vmaf-perShot.md)), which wraps TransNet V2. A missing or
   failing binary degrades to one shot spanning the clip.
2. **Uniform-window splitter.** Any shot longer than `--max-shot-duration` is
   sliced into equal sub-shots, so an under-cutting detector (fades, short
   clips) still yields a usable timeline.
3. **Per-shot bisect.** Each shot is extracted to raw YUV and run through the
   CRF bisect against `--target-vmaf`.
4. **Plan emission.** The recommendations become segment and concat commands.

### tune-per-shot flags

`--src` is required. Source geometry:

| Flag | Default | Description |
|------|---------|-------------|
| `--src` | — | **Required.** Reference video: raw YUV, or any FFmpeg-readable container. |
| `--width` | auto-probed | Source width. Required for raw YUV (`.yuv` / `.raw`); auto-probed via `ffprobe` for containers. |
| `--height` | auto-probed | Source height. Same rule as `--width`. |
| `--pix-fmt` | `yuv420p` | Source pixel format. |
| `--framerate` | auto-probed | Source framerate. Falls back to `24.0` when the probe yields nothing. |
| `--bitdepth` | `8` | Source YUV bit depth: `8`, `10` or `12`. |
| `--total-frames` | `0` | Frame count for the single-shot fallback when `vmaf-perShot` is unavailable. |

Shot detection:

| Flag | Default | Description |
|------|---------|-------------|
| `--per-shot-bin` | `vmaf-perShot` | Path to the shot-detector binary. |
| `--scene-threshold` | detector default (12.0) | Override the detector's mean-absolute-luma-delta cut threshold. Lower yields more shots. |
| `--max-shot-duration` | `2.0` | Uniform-window splitter, in seconds. `0` disables it. |

Tuning:

| Flag | Default | Description |
|------|---------|-------------|
| `--target-vmaf` | `92` | Target pooled-mean VMAF per shot. |
| `--encoder` | `libx264` | Codec adapter (see [Supported codecs](#supported-codecs)). |
| `--preset` | codec default (`medium`) | Preset for the bisect encodes. |
| `--crf-min` / `--crf-max` | codec absolute window | Inclusive bisect search bounds. Pass both or neither. |
| `--max-iterations` | `8` | Maximum encode+score rounds per shot. |
| `--vmaf-model` | `vmaf_v1.0.16_3d0h` | Model passed to the `vmaf` binary. |
| `--neg` | off | Route the model to its NEG variant. There is no NEG counterpart to any `vmaf_v1.0.16_*` model, so `--neg` also selects the v0.6.1 generation (`vmaf_v0.6.1neg`). See [vmaf-neg.md](../metrics/vmaf-neg.md). |
| `--score-backend` | `auto` | libvmaf backend: `auto`, `cpu`, `cuda`, `sycl`, `hip`, `metal`. An explicit backend that `vmaf --list-backends` does not report usable fails fast rather than silently downgrading. |
| `--vmaf-bin` / `--ffmpeg-bin` | `vmaf` / `ffmpeg` | Binary paths. |
| `--workdir` | `$VMAFTUNE_WORKDIR` or OS temp | Scratch space for encode and decode artefacts (ADR-0598). Raw YUV decodes are large; point this at a volume with room. |
| `--max-concurrent-decodes` | `1` | Concurrent reference-YUV decodes (ADR-0577). `1` is safest on space-constrained volumes. |

Output:

| Flag | Default | Description |
|------|---------|-------------|
| `--plan-out` | stdout | Destination for the JSON plan. |
| `--output` | `per_shot_encode.mp4` | Final concatenated encode path named inside the plan. |
| `--segment-dir` | `<output dir>/segments` | Directory the segment commands write into. |
| `--script-out` | — | Also write the plan as a copy-paste shell script. |

```bash
vmafx-tune-go tune-per-shot \
  --src src.mp4 \
  --target-vmaf 92 \
  --encoder libx264 \
  --plan-out plan.json \
  --segment-dir ./segments \
  --output final.mp4
```

### Plan JSON schema

The plan is byte-compatible with `vmaf-tune tune-per-shot`: keys are sorted,
floats keep Python's `repr()` form, and a shot whose predicate never measured a
bitrate carries `null` rather than `NaN`.

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

### Supported codecs

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

The Python registry carries seven more adapters: `av1_nvenc`, `av1_qsv`,
`av1_amf`, the four VideoToolbox encoders, `libvvenc` and `libvpx-vp9`. They
have no Go encoder implementation yet, so `--encoder` rejects them by name and
points at the Python binary.

## sidecar: local predictor sidecar

`sidecar` trains and inspects a bias-correction term on top of the shipped
predictor:

$$
\mathrm{vmaf}_{\mathrm{sidecar}} = \mathrm{vmaf}_{\mathrm{predictor}} + \mathrm{correction}_{\mathrm{sidecar}}(\mathit{features})
$$

The shipped predictor is never mutated, so model upgrades stay deterministic
and reproducible across hosts.

```text
vmafx-tune-go sidecar <status|predict|record|batch-record> [flags]
```

Flags shared by every nested subcommand:

| Flag | Default | Description |
|------|---------|-------------|
| `--codec` | `libx264` | Codec bucket for the sidecar state. Must be one of the 19 registered codec names; an unknown name is a usage error whose message lists them. |
| `--cache-dir` | `${XDG_CACHE_HOME:-~/.cache}/vmaf-tune/sidecar` | Sidecar cache root. A relative path stays relative in `state_path`. |
| `--predictor-version` | `predictor_v1` | Predictor-version namespace. |
| `--model` | unset | Optional ONNX predictor; see [ONNX predictor models](#onnx-predictor-models). Unset uses the analytical fallback. |
| `--json` | `false` | Emit machine-readable JSON instead of the one-line text form. |

Nested subcommands:

| Subcommand | Extra flags | Purpose |
|------------|-------------|---------|
| `status` | — | Print state metadata: codec, host UUID, state path, predictor version, update count, residual RMS. |
| `predict` | `--features-json`, `--crf` | Predict VMAF with the correction folded in. Reports the base score, the correction, and the sum. |
| `record` | `--features-json`, `--crf`, `--observed-vmaf`, `--no-persist` | Fold one observed encode result into the fit. |
| `batch-record` | `--captures-jsonl` | Fold a JSONL capture file, one observation per row, persisting once at the end. |

Inspect, train from a capture log, then predict:

```bash
vmafx-tune-go sidecar status --json
vmafx-tune-go sidecar batch-record --captures-jsonl captures.jsonl --json
vmafx-tune-go sidecar predict --features-json shot.json --crf 26 --json
```

### sidecar status

`sidecar status` prints the state metadata for one codec bucket: the anonymous
host UUID, the on-disk state path, the predictor-version namespace, the number
of folded-in captures, and the RMS of the buffered residuals (the drift
signal).

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

Without `--json` the same fields print as one line:

```text
codec=libx264 predictor_version=predictor_v1 updates=0 residual_rms=0.000000 state=/home/u/.cache/vmaf-tune/sidecar/predictor_v1/libx264/state.json
```

### sidecar predict

`sidecar predict` predicts VMAF for one shot at one CRF with the correction
applied.

| Flag | Description |
|------|-------------|
| `--features-json` | **Required.** Path to a JSON object carrying the shot's feature values. |
| `--crf` | **Required.** CRF to predict at. |

```bash
vmafx-tune-go sidecar predict --features-json shot.json --crf 26 --json
```

The payload (schema `vmaf-tune-sidecar-predict/v1`) carries `base_vmaf` (the
bare predictor), `correction`, and `sidecar_vmaf` (the sum, clamped to
`[0, 100]`), plus `codec`, `crf` and `n_updates`. The text form is
`base=<b> correction=<c> sidecar=<s> updates=<n>` with six decimals.

### sidecar record

`sidecar record` folds one observed VMAF measurement into the ridge fit.

| Flag | Description |
|------|-------------|
| `--features-json` | **Required.** Path to the shot's feature JSON. |
| `--crf` | **Required.** CRF the observation was measured at. |
| `--observed-vmaf` | **Required.** Observed libvmaf score for the encode. |
| `--no-persist` | Update in memory only; mainly useful for tests. |

```bash
vmafx-tune-go sidecar record \
  --features-json shot.json \
  --crf 26 \
  --observed-vmaf 91.75 \
  --json
```

The residual is computed against the bare predictor, never against the
sidecar-corrected value, so repeated captures converge rather than compounding.
The payload (schema `vmaf-tune-sidecar-record/v1`) is the `status` payload plus
`crf`, `observed_vmaf`, `base_vmaf` and `residual`
(`observed_vmaf - base_vmaf`); the text form is
`recorded updates=<n> residual=<r> state=<path>`.

### sidecar batch-record

`sidecar batch-record` folds a whole JSONL capture file into the fit, one
observation per line.

| Flag | Description |
|------|-------------|
| `--captures-jsonl` | **Required.** Path to the JSONL capture file. |

```bash
vmafx-tune-go sidecar batch-record --captures-jsonl captures.jsonl --json
```

Each line is a JSON object carrying the feature fields (either at the top level
or nested under a `features` key) plus `crf` and `observed_vmaf`. Lines are
split on `\n`, `\r\n` or a lone `\r` (CPython's universal-newline rule) with no
length limit; blank lines are ignored but still numbered.

A malformed row is not fatal. That covers a row that is not a JSON object, a
missing or non-numeric required field, and a `crf` string that is not an
integer literal. It is reported as
`vmafx-tune sidecar batch-record: skip line <n>: <reason>` on `stderr`, counted
in `rows_skipped`, and the run still succeeds. That is deliberate: a capture
log is often partially corrupt after an interrupted run, and losing the good
rows to one bad line would be worse.

State is written once at the end, and only when at least one row was recorded.
The payload (schema `vmaf-tune-sidecar-batch-record/v1`) is the `status`
payload plus `rows_recorded` and `rows_skipped`; the text form is
`recorded=<rows> skipped=<rows> updates=<n> state=<path>`.

### Feature JSON

`--features-json` takes an object of shot features, or a `{"features": {...}}`
wrapper so a capture row can carry `crf` and `observed_vmaf` alongside. The
four `probe_*` fields are required (a zero probe bitrate would train the fit on
a fabricated complexity barometer); everything else defaults to `0`. Values may
be JSON numbers or numeric strings (`"2400"`), as CPython's `float()` accepts.

| Key | Required | Meaning |
|-----|----------|---------|
| `probe_bitrate_kbps` | yes | Average bitrate over the probe encode. |
| `probe_i_frame_avg_bytes` | yes | Mean I-frame size. |
| `probe_p_frame_avg_bytes` | yes | Mean P-frame size. |
| `probe_b_frame_avg_bytes` | yes | Mean B-frame size (0 for codecs without B-frames). |
| `saliency_mean`, `saliency_var` | no | Saliency signals; 0 when unavailable. |
| `frame_diff_mean`, `y_avg`, `y_var` | no | FFmpeg `signalstats` aggregates. |
| `shot_length_frames`, `fps`, `width`, `height` | no | Structural metadata. |

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

### State and privacy

State lives at:

```text
${XDG_CACHE_HOME:-~/.cache}/vmaf-tune/sidecar/
  host-uuid                                    # random 128-bit token
  <predictor-version>/<codec>/state.json       # ridge weights + inverse Gram
```

The host UUID is drawn from a CSPRNG on first use. It is never derived from a
MAC address, hostname, `/etc/machine-id`, CPUID, or any other
machine-identifying signal.

A predictor-version or schema mismatch on load discards the fit and resets to
cold start, keeping only the host UUID. That is what makes a shipped-model
upgrade safe: a stale correction can never be replayed against a refreshed
predictor. At cold start the weights are zero, so the correction is exactly
`0.0` and the sidecar returns the bare predictor's value untouched.

A corrupt `state.json` also cold-starts, including one whose `weights` or
`a_inv` carry `null` (the residue of a NaN capture). The corrupt file is left
in place so you can inspect it.

### Sidecar exit codes and diagnostics

The four subcommands follow the Python `vmaf-tune sidecar` exit contract:

| Status | Meaning |
|--------|---------|
| `0` | Success, including a `batch-record` run in which every row was skipped. |
| `1` | An I/O failure the Python CLI does not catch either: the cache directory cannot be created, the host UUID or `state.json` cannot be written, or stdout cannot be written. |
| `2` | A usage or validation failure: an unknown or unparseable flag, a missing required flag, an unknown `--codec`, an unresolvable `--model`, a `--features-json` that cannot be read / is not a JSON object / lacks a required key, or a `--captures-jsonl` that cannot be read. |

Diagnostics go to `stderr` and are not byte-identical to Python's: cobra
prefixes them with `Error:` where Python prints `vmaf-tune sidecar <cmd>:`, the
`batch-record` skip lines carry the `vmafx-tune` prefix, and the reason text is
Go's rather than CPython's exception message. Nothing is written to `stdout` on
failure.

### Byte compatibility with vmaf-tune sidecar

For the same inputs, every `stdout` payload (JSON and text form) and every byte
of `state.json` written by the Go binary is identical to the Python CLI's. The
analytical predictor, the Sherman–Morrison update, and the
`json.dumps(..., indent=2, sort_keys=True)` rendering are reproduced to the
last bit, and the atomic write leaves the same single `state.json` behind (no
`.tmp` residue).

`cmd/vmafx-tune/cmd/testdata/sidecar/` holds fixtures dumped from the Python
CLI by `regen.sh` (pinned host UUID, relative `--cache-dir`), and
`TestSidecarPythonParity` replays the same 23-step operator sequence: cold
`status`, three `record`s, two `batch-record` loads, warm `status` and
`predict`, a `--no-persist` record, a second codec bucket, and nine error
paths. It requires identical `stdout`, identical `state.json` snapshots and
identical exit statuses.

Known, deliberate differences:

- A capture row containing the non-standard JSON tokens `NaN` / `Infinity` is
  skipped by the Go binary and counted in `rows_skipped`. CPython's
  `json.loads` accepts the tokens: a `NaN` feature then silently skips the
  update while still counting the row as recorded, and a `NaN` `observed_vmaf`
  poisons the ridge weights (they persist as `null` and the next load
  cold-starts).
- Hand-edited state files outside the shape either writer produces are not
  guaranteed to load identically (for example a numeric string where CPython's
  `float()` would coerce and Go's decoder will not).
- `--model` is resolved differently; see
  [ONNX predictor models](#onnx-predictor-models).

## ONNX predictor models

The two binaries interpret `--model` (on `predict`, `sidecar` and `auto`)
differently, and the Python behaviour itself depends on the host.

- **Python** takes a filesystem path to `predictor_<codec>.onnx`. With
  `onnxruntime` importable, a missing file raises and the CLI exits `2`;
  without it, the flag is silently ignored and the analytical curve is used
  (exit `0`).
- **Go** resolves the value through the model registry (`pkg/ai`): an absolute
  path that exists, else `<model-dir>/<name>.onnx`, else `<model-dir>/<name>`,
  where the model dir is `$VMAFX_MODEL_DIR` or `/usr/local/share/vmafx/model`.
  An unresolvable name exits `2` on `sidecar`. Inference then goes through the
  `vmafx-ort-runner` subprocess ([vmafx-ort-runner.md](vmafx-ort-runner.md)),
  which this repository builds from `cmd/vmafx-ort-runner` (ADR-1134; the dev
  container and the Go CI job both build it).

When the runner is absent from `PATH`, or present but linked against a libvmaf
built without ONNX Runtime (exit 3), the predictor logs one warning carrying the
runner's error and falls back to the analytical curve. The Python takes the same
fallback without `onnxruntime`, but silently.

Omit `--model` to use the analytical fallback. It is the default and the only
path the parity fixtures exercise, and the two binaries agree byte for byte on
it.

## Exit codes

Every subcommand exits `0` on success. A usage failure exits `2` on every
subcommand, as argparse does in the Python CLI: an unknown or unparseable flag,
a missing required flag (including `--src` of `auto` without `--smoke`, the
input files of `report`, `--target-vmaf` of `prefilter`, and the source, size
and preset flags of `recommend` without `--from-corpus`). Other failures exit
`1` unless the table names another status.

| Subcommand | Exit | Meaning |
|------------|------|---------|
| `benchmark`, `encode-profile`, `sidecar` | `2` | Also every validation failure the command detects itself: a missing input file, a filter that matches no recommendation, a baseline encoder absent from the corpus, an unknown `--codec`, or an unreadable feature or capture file on `sidecar`. |
| `sidecar` | `1` | The cache directory, host UUID or `state.json` could not be written (an uncaught `OSError` in Python). See [Sidecar exit codes and diagnostics](#sidecar-exit-codes-and-diagnostics). |
| `encode-profile` | FFmpeg's own | A failed encode propagates FFmpeg's exit status. |
| `fast` | `2` / `3` | See [fast exit codes](#fast-exit-codes). |
| `predict` | `2` | The `fall_back` verdict, or an unreadable `--saliency-model`. |
| `prefilter` | `2` | An invalid CRF range, a failed search, or a live-loop prerequisite that is missing (`--src`, geometry, or the `pelorus_deband_vulkan` filter). |
| `recommend-saliency` | `2` / encode status | `2` when the encoder has no ROI dispatch and no opt-in; an encode failure carries the encode's exit status. |
| `ladder` | `2` | No `(resolution, target)` cell produced a scored encode. |
| `auto` | `2` | An empty `--allow-codecs`. |

`TestEveryCommandRejectsUnknownFlagWithUsageStatus` and
`TestMissingRequiredFlagExitsWithUsageStatus` in
`cmd/vmafx-tune/cmd/required_flags_test.go` hold every subcommand to the usage
status.

## Configuration and logging

`vmafx-tune-go` runs each subcommand inside the golusoris `clikit` (cobra + fx)
framework. The framework injects a structured `*slog.Logger` and a config tree
into every subcommand, so run diagnostics (sweep start and finish, ladder build
summary, report rendering) are emitted as structured log lines on `stderr`,
while subcommand output (JSON, Markdown, HTML) goes to `stdout` or the
`--output` file. This keeps machine-readable output separate from logs when you
pipe `stdout`.

Configuration is read from environment variables under the `VMAFX_` prefix.
golusoris maps each underscore in the variable name to a config-path delimiter
(`VMAFX_LOG_LEVEL` → `log.level`).

<!-- BEGIN GENERATED: vmafx-api environment vmafx-tune (scripts/codegen/vmafx-api.py) -->

| Variable | Key | Type | Default | Chart value | Description |
|---|---|---|---|---|---|
| `VMAFX_MODEL_DIR` | read directly | path | `/usr/local/share/vmafx/model` |  | Directory searched for `<name>.onnx` when `--model` names no file (`predict`, `sidecar`, `auto`). |
| `VMAFX_TUNE_ENCODE_TIMEOUT` | read directly | duration | `60m` |  | Deadline of each `ffmpeg` encode; a value that does not parse keeps the default. |
| `VMAFX_TUNE_PROBE_TIMEOUT` | read directly | duration | `30s` |  | Deadline of each `ffprobe` call; a value that does not parse keeps the default. |
| `VMAFX_TUNE_SCORE_TIMEOUT` | read directly | duration | `30m` |  | Deadline of each `vmaf` run; a value that does not parse keeps the default. |
| `VMAFTUNE_WORKDIR` | read directly | path | OS temp directory |  | Scratch parent of `tune-per-shot` when `--workdir` is not given; used only when it can be created and written. |
| `VMAFTUNE_VAAPI_DEVICE` | read directly | path | first Intel render node, else `/dev/dri/renderD128` |  | VAAPI render node of QSV encodes; `auto` means unset. |
| `VMAFTUNE_SALIENCY_FALLBACK_OK` | read directly | `1` | off |  | `1` lets `recommend-saliency` run a plain encode on an encoder without ROI support instead of exiting 2. |
| `XDG_CACHE_HOME` | read directly | path | `~/.cache` |  | Parent of the `vmaf-tune/sidecar` cache when `--cache-dir` is not given. |
| `VMAFX_LOG_LEVEL` | `log.level` | string | `info` |  | Log level: `debug`, `info`, `warn` or `error`, any case; an unknown value gives `info`. |
| `VMAFX_LOG_FORMAT` | `log.format` | string | `auto` |  | Log handler: `auto` (tint on a terminal, else JSON), `tint` or `json`; logs go to stderr. |
| `VMAFX_OTEL_ENABLED` | `otel.enabled` | bool | `true` |  | OpenTelemetry master switch; `false` installs no-op providers even with an endpoint. |
| `VMAFX_OTEL_ENDPOINT` | `otel.endpoint` | `host:port` | _(unset)_ |  | OTLP/gRPC collector (`otel-collector:4317`); wins over `OTEL_EXPORTER_OTLP_ENDPOINT`. Neither set: no export ([OpenTelemetry](../observability/otel.md)). |
| `VMAFX_OTEL_INSECURE` | `otel.insecure` | bool | `true` |  | Plaintext gRPC to the collector; `false` dials with TLS. |
| `VMAFX_OTEL_SERVICE_NAME` | `otel.service.name` | string | `OTEL_SERVICE_NAME`, else the binary name |  | `service.name` resource attribute. |
| `VMAFX_OTEL_SERVICE_VERSION` | `otel.service.version` | string | the build version |  | `service.version` resource attribute. |
| `VMAFX_OTEL_SERVICE_NAMESPACE` | `otel.service.namespace` | string | _(unset)_ |  | `service.namespace` resource attribute. |
| `VMAFX_OTEL_SAMPLE_RATIO` | `otel.sample.ratio` | number | `1.0` |  | Parent-based trace sample ratio in `[0, 1]`; `OTEL_TRACES_SAMPLER` and its argument are not read. |
| `VMAFX_OTEL_EXPORT_TRACES` | `otel.export.traces` | bool | `true` |  | Export traces. |
| `VMAFX_OTEL_EXPORT_METRICS` | `otel.export.metrics` | bool | `true` |  | Export metrics. |
| `VMAFX_OTEL_EXPORT_LOGS` | `otel.export.logs` | bool | `true` |  | Export the logs signal; application logs are not bridged to it today. |
| `OTEL_SERVICE_NAME` | read directly | string | _(unset)_ |  | `service.name` when `VMAFX_OTEL_SERVICE_NAME` is unset (OTel standard). |
| `OTEL_SDK_DISABLED` | read directly | string | _(unset)_ |  | `true` (exactly) installs no-op providers (OTel standard). |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | read directly | URL | _(unset)_ |  | Collector as a URL (`http://host:4317`) when `VMAFX_OTEL_ENDPOINT` is unset; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for traces; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for metrics; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_LOGS_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for logs; set, export is on (OTel standard). |
| `POD_NAME` | read directly | string | _(unset)_ |  | Pod name (Kubernetes downward API), added to log lines and OTel resources as `k8s.pod.name`. |
| `POD_NAMESPACE` | read directly | string | _(unset)_ |  | Pod namespace, added as `k8s.namespace.name`. |
| `POD_IP` | read directly | string | _(unset)_ |  | Pod IP, added as `k8s.pod.ip`. |
| `NODE_NAME` | read directly | string | _(unset)_ |  | Kubernetes node name, added as `k8s.node.name`. |
| `SERVICE_ACCOUNT` | read directly | string | _(unset)_ |  | Service account name, added as `k8s.serviceaccount.name`. |

<!-- END GENERATED: vmafx-api environment vmafx-tune -->

```bash
# Quiet the per-run INFO diagnostics; keep warnings and errors.
VMAFX_LOG_LEVEL=warn vmafx-tune-go report results.json

# Force JSON logs for machine ingestion regardless of TTY.
VMAFX_LOG_FORMAT=json vmafx-tune-go compare --reference src.mp4 --targets 90
```

## Python-only flags

Every `vmaf-tune` subcommand is ported. A few individual flags still need the
Python implementation, because they depend on in-process ONNX inference or on
importing a Python callable at runtime. Each is accepted by the Go parser and
fails with a message naming the fallback rather than being silently ignored.

| Flag | Subcommand | Why | Use instead |
|------|-----------|-----|-------------|
| `--fast-nr` | `tune-per-shot` | NR early-elimination needs an ONNX forward pass per bisect midpoint (`nr_metric_v1` through `onnxruntime`) | `vmaf-tune tune-per-shot --fast-nr` |
| `--predicate-module` | `tune-per-shot` | Imports an arbitrary Python `MODULE:CALLABLE` at runtime; the Go equivalent is the `pershot.PredicateFn` seam, available to library callers | `vmaf-tune tune-per-shot --predicate-module` |

`recommend-saliency --saliency-aware` and `predict --use-saliency` are accepted
by the Go binary. When the saliency session cannot be built,
`recommend-saliency` proceeds without an ROI map (the report's `saliency_aware`
field then reads `false`), and `predict` logs a warning and degrades saliency
moments to 0.0, matching the Python behaviour. A real saliency-biased encode
needs an ONNX forward pass: use `vmaf-tune recommend-saliency
--saliency-aware` for it (see [Saliency ONNX inference](#saliency-onnx-inference)).

### Corpus fields not carried by recommend

`recommend`'s encode-driven path writes the same schema-v3 corpus JSONL the
`corpus` subcommand does, and every key is present. Five corpus features are
not carried by this group's port, and their fields hold the same zero or empty
values the Python emits when the feature is unavailable, so a reader filters on
them exactly as it already does:

- the content-addressed encode cache (ADR-0298),
- HDR detection (ADR-0295),
- TransNet-V2 shot metadata (`shot_count` stays 0),
- sample-clip windowing (`clip_mode` stays `full`),
- the encoder-internal pass-1 stats (the ten `enc_internal_*` columns stay 0.0,
  which is what the Python aggregator returns for an empty frame list).

These belong to the `corpus` port.

## Python vs Go differences

The flag surface of `recommend-saliency`, `tune-per-shot`, `fast`, `corpus`,
`benchmark` and `encode-profile` matches Python flag for flag. The differences
below were checked against the Python argparse tree
(`tools/vmaf-tune/src/vmaftune/cli.py`) and the Go flag registrations in
`cmd/vmafx-tune/cmd/`.

| Subcommand | Python | Go |
|------------|--------|----|
| `compare` | `--src`, `--encoders`, `--target-vmaf` (92.0) and `--target-vmafs` (`94,96,97,98`); `--format` `markdown` default with `json`, `csv`, `html`, `both` | `--reference`, `--codecs`, `--targets` (`85`); `--format` `json` default with `markdown` only |
| `compare` | Also `--preset`, `--width`, `--height`, `--pix-fmt`, `--framerate`, `--duration`, `--crf-sweep`, `--no-bisect`, `--neg`, `--vmaf-model`, `--score-backend`, `--sample-clip-seconds`, `--workdir`, `--max-workers`, `--no-parallel`, `--vaapi-device`, `--json-sidecar`, `--fast-nr`, `--predicate-module` and more | None of these; Go-only: `--work-dir`, `--crf-lo`, `--crf-hi`, `--max-iter`, `--ffmpeg`, `--vmaf` |
| `ladder` | `--src`, `--encoder` (19 adapters), `--target-vmafs`; `--format` `hls` default with `dash`, `json` | `--reference`, `--codec` (10 encoders), `--targets` (`75,85,95`); `--format` `json` default with `markdown` only |
| `ladder` | Also `--crf-sweep`, `--src-width`, `--src-height`, `--quality-tiers`, `--spacing`, `--rung-overlap-threshold`, `--with-uncertainty`, `--uncertainty-sidecar`, `--neg`, `--score-backend`, `--workdir` | None of these; Go-only: `--resolutions`, `--max-rungs`, `--min-bitrate-gap`, `--crf-lo`, `--crf-hi`, `--max-iter` |
| `report` | Inputs by flag: `--compare-json`, `--ladder-json`, `--per-shot-json`; `--format` `html` default with `markdown`, `both`; also `--assets-dir`, `--src`, `--target-vmaf` | Positional input files (`compare` / `ladder` JSON only); `--format` `markdown` default with `html` |
| `recommend` | `--neg`, `--two-pass` | Not present |
| `prefilter` | `--neg` | Not present |
| `predict` | No `--vmaf-bin` | `--vmaf-bin` |
| `auto` | No `--model` | `--model` |

Only the three oldest ports (`compare`, `ladder`, `report`) renamed their
inputs; `fast`, `auto` and `tune-per-shot` use `--src` in both binaries.

## Known gaps

Two behaviours are not at full parity with Python. Both are documented here
rather than hidden behind a silent degradation.

### Saliency ONNX inference

`recommend-saliency --saliency-aware` falls back to a plain encode. Everything
around the model is ported and tested against the Python; only the single
forward pass is missing:

- Go has no in-process ONNX Runtime in this module. The fork's bridge
  (`pkg/ai`, ADR-0713) shells out to `vmafx-ort-runner` and passes the input
  tensor as a JSON array in argv. That works for the per-shot predictor's 14
  floats, but it cannot carry saliency's 3×H×W input, which is 6.2 million
  floats (about 75 MB of JSON) for a 1080p frame.
- The runner itself is built in this repository (`cmd/vmafx-ort-runner`,
  ADR-1134; see [vmafx-ort-runner.md](vmafx-ort-runner.md)) and serves the
  predictor path. It has no transport for tensors that do not fit in argv.

Either of two changes unblocks it: a cgo ONNX Runtime binding (an ADR-level
decision, since the binary currently builds without cgo), or a runner protocol
that streams tensors over stdin (a protocol extension of the in-tree runner and
`pkg/ai`, not a new dependency).

The per-shot predictor ONNX (`predict --model`) does route through `pkg/ai`,
because its 14-float input fits argv comfortably. It degrades to the
analytical curve when the runner is absent from `PATH` or linked against a
libvmaf built without ONNX Runtime (exit 3), and the log says which.

### TPE sampler trajectory

`prefilter` implements TPE natively (Bergstra et al. 2011 §4, the construction
Optuna follows) rather than depending on a Go Optuna port that would pull gorm
plus the MySQL, Postgres and cgo-SQLite drivers into a one-shot CLI. The search
space, the objective, the emitted JSON and per-seed reproducibility are
identical to the Python. The trial-by-trial trajectory for a given seed is not,
and cannot be, because the two use different RNG streams.

`fast` uses `goptuna` instead and has the weaker reproducibility described
under [fast](#fast-proxy-tpe-and-gpu-verify-recommend).

## Architecture

The CLI root and every subcommand are built with the golusoris `clikit`
(cobra + fx) framework (ADR-1119): `clikit.New` builds the root,
`clikit.Command` builds each subcommand, and a thin `withGolusoris` adapter
boots a one-shot fx graph per invocation so the command receives an injected
`*slog.Logger` and config, runs to completion, and propagates its error as the
process exit code.

The Go binary uses an adapter pattern. Each encoder shells out to `ffmpeg`
(no `libavcodec` cgo dependency), and each scorer shells out to the `vmaf`
CLI. Each package has one implementation (ADR-1137).

### Core packages

| Package | Role |
|---------|------|
| `pkg/encoder/` | `Encoder` interface plus the software and hardware encoder implementations, the codec-adapter policy table (`Adapter`: preset vocabulary, quality windows, per-codec argv shape), `AdapterEncoder` and `ProbeSource`. `EncodeParams.InputArgs` carries ffmpeg input-side options so raw-YUV sources (`-f rawvideo -pix_fmt -s -r`) and sample clips (input-side `-ss` / `-t`, for fast-seek) work; `EncodeParams.OutputPath` pins a deterministic destination and `EncodeResult.OutputSizeBytes` reports the encode size for size-over-duration bitrate maths. |
| `pkg/bisect/` | Stateless `Run(src, enc, scoreFunc, params)`. The score function is injectable, so unit tests need no live `vmaf` binary. `YUVScoreFunc` decodes a containerised distorted file to raw YUV and invokes `vmaf` with full geometry, model and backend flags. |
| `pkg/ladder/` | `Build(src, encoder, Params)`: convex hull (`upperConvexHull`), knee selection (`selectRenditions`), min-bitrate-gap filter. |
| `pkg/report/` | `EmitJSON` / `EmitMarkdown` renderers (single run) and `RenderMarkdownMulti` / `RenderHTMLMulti` (multi-file report rendering). |
| `pkg/pershot/` | Shot detection via `vmaf-perShot` with the single-shot fallback, the uniform long-shot splitter, per-shot tuning, and encoding-plan construction and JSON emission. |
| `pkg/scorebackend/` | libvmaf backend detection and strict selection, ported from the selection half of `vmaftune/score_backend.py`. `Detect` intersects what the local `vmaf --help` advertises with what `nvidia-smi` / `sycl-ls` / `rocminfo` report; `Select` honours `auto` through a fallback chain and never silently downgrades an explicit request. |
| `pkg/fast/` | The fast path: `Recommend` (the flow), `RunTPE` (the goptuna-backed search), `NewSamplePredictor` / `NewVerifier` (the probe and verify pipelines) and `ORTProxy` (the `fr_regressor_v2` seam). |
| `pkg/conformal/` | Distribution-free prediction intervals for the VMAF predictor (split conformal and CV+ / jackknife+), ported from `vmaftune/conformal.py`. The JSON sidecar is byte-compatible with the Python writer, so a calibration produced by either implementation loads in the other. Wired into `predict` (`--with-uncertainty`, `--calibration-sidecar`). |

### Shared layers

| Package | Role |
|---------|------|
| `pkg/codecadapter/` | The 19-codec adapter registry (ADR-0237): quality knob and window, preset mapping onto each encoder's native axis, validation rule, probe argv, two-pass argv, ffmpeg argv slice. The encode driver never branches on codec identity. |
| `pkg/ffencode/` | The encode driver the tuning subcommands share: raw-YUV geometry, named presets, injected extra params, sample-clip windowing. Distinct from `pkg/encoder`, which models the narrower Stage-1 bisect abstraction. |
| `pkg/scorecli/` | The libvmaf CLI driver with explicit geometry, the backend selector and the canonical-6 pooled aggregates. |
| `pkg/predictor/` | `ShotFeatures`, the per-codec analytical curve with the optional ONNX session, the binary-search CRF inversion (`PickCRF`), feature extraction and the validation harness. |
| `pkg/saliency/` | The full saliency ROI pipeline: YUV to ImageNet tensor, four temporal aggregators, QP mapping, per-block reduce, five sidecar formats. |
| `pkg/prefilter/` | The frozen Pelorus knob contract plus a native TPE sampler. |
| `pkg/recommend/`, `pkg/uncertainty/`, `pkg/corpusrow/` | The predicate pickers, confidence bands and the schema-v3 corpus row. |
| `pkg/corpus/` | The Phase A orchestrator: encode and score drivers, the pass-1 encoder-stats parser, HDR detection and codec-arg dispatch, shot metadata, scoring-backend selection, the JSONL reader and writer, and the coarse-to-fine search. It also ports CPython's Neumaier-compensated `sum()` and `statistics.pstdev()` so the aggregate columns match to the last bit. |
| `pkg/benchmark/` | Corpus loading (`LoadCorpusJSONL`), per-encoder summarisation (`Summarize`) and the three renderers. No subprocess at all. |
| `pkg/encodeprofile/` | Profile loading from JSON / HTML / Markdown, recommendation selection, `EncodeRequest` construction, FFmpeg argv composition and the encode driver, with an injectable `Runner` seam so tests never spawn ffmpeg. |
| `pkg/hdr/` | HDR detection from ffprobe colour metadata plus the per-codec HDR flag dispatch. |
| `pkg/pyjson/` | The one CPython-compatible JSON encoder in the tree ([ADR-1137](../adr/1137-go-dedup-tune-shadow.md)). It reproduces `json.dumps(..., indent=2, sort_keys=True)` and `jsonio.dumps_strict` byte for byte: bare `NaN` / `Infinity` tokens, `repr()`-style float rendering, `ensure_ascii` escaping. Go's `encoding/json` differs on key ordering, HTML escaping, non-ASCII escaping and float formatting (`float64(92)` renders `92` in Go and `92.0` in CPython), so a shared encoder keeps the payloads diff-clean against the Python originals. |
| `pkg/pymath/` | Correctly rounded `Exp2` / `Log10` kernels that keep `estimated_bitrate_kbps` and `estimated_vmaf` on the platform-libm value CPython emits (Go's `math.Pow` / `math.Log10` land a ULP away; the package docs record the measured residual). |
| `pkg/ai/` | The model registry and the `vmafx-ort-runner` subprocess bridge (`Registry.Infer`). |

### The auto and sidecar stack

| Package | Role |
|---------|------|
| `pkg/tune/auto/` | The Phase F decision tree: source probing, the ten short-circuit predicates, the recipe table, the confidence policy, winner selection and the plan emitter. |
| `pkg/tune/sidecar/` | The online-ridge bias-correction model (Sherman–Morrison rank-1 updates) and its cache-dir persistence. The port briefly produced a second implementation (`pkg/sidecar/`); it was removed because nothing imported it. |
| `pkg/tune/executor/` | `--execute` mode: the libvmaf CLI driver and the JSONL results log. Its ffmpeg argv is `pkg/ffencode`'s under the executor's name. |
| `pkg/tune/{predictor,codec,pyjson}/` | Thin aliases of `pkg/predictor`, `pkg/codecadapter` and `pkg/pyjson`. The `sidecar` subcommand and `pkg/tune/sidecar` still import them; do not add code to them. |

ADR-1125 records which consumer owns which copy of the shared layers. For the
migration rationale see [ADR-0705](../adr/0705-vmafx-tune-go-stage1.md),
[ADR-0730](../adr/0730-vmafx-tune-go-stage2.md) (Stage 2),
[ADR-0770](../adr/0770-vmafx-tune-go-stage4-report.md) (Stage 4) and
[ADR-0702](../adr/0702-vmafx-phase4-language-modernization.md) (the Phase 4
umbrella).

### Tests

`cmd/vmafx-tune/cmd/*_test.go` covers each subcommand; `pkg/*/..._test.go`
covers the packages. Python-parity fixtures live under
`cmd/vmafx-tune/cmd/testdata/` and in `pkg/codecadapter/testdata/`. Run the
Go tests with `go test ./cmd/vmafx-tune/... ./pkg/...`.

## History

### Port stages

The Go port landed in stages. Every row below is merged unless noted.

| Stage | Scope | ADR |
|-------|-------|-----|
| Stage 1 | `compare` subcommand, libx264 / libx265, single and multi-target bisect | ADR-0705 |
| Stage 2 | `ladder` subcommand, hardware encoders (NVENC, QSV, AMF), convex hull and knee selection | ADR-0730 |
| Stage 3 | `pkg/conformal` and downscale plumbing, as recorded in ADR-0770. The `ladder` sampler still bisects at the native source resolution (see the note under [ladder](#ladder-per-title-abr-ladder)). | ADR-0770 |
| Stage 4 | `report` subcommand, Markdown and HTML rendering | ADR-0770 |
| golusoris | Migrate the CLI root and subcommands onto the golusoris `clikit` (cobra + fx) framework; `VMAFX_`-prefixed config and injected `slog` | ADR-1119 |
| ML-driven | `recommend`, `predict`, `recommend-saliency`, `prefilter`; codec-adapter registry, encode and score drivers, predictor, saliency pipeline, native TPE | — |
| Encoder introspection | `benchmark` and `encode-profile`; `pkg/benchmark`, `pkg/codecadapter`, `pkg/encodeprofile`, `pkg/pyjson` | ADR-0643 (profile contract) |
| Stage 5 (corpus, auto, sidecar) | `corpus`, `auto` and `sidecar`; `pkg/codecadapter`, `pkg/corpus`, `pkg/pyjson`, `pkg/tune/predictor`, `pkg/tune/sidecar`, `pkg/tune/auto`, `pkg/tune/executor` | ADR-1125 ([#1153](https://github.com/VMAFx/vmafx/pull/1153)) |
| Stage 5 (per-shot) | `tune-per-shot`; `pkg/pershot`, `pkg/scorebackend`, the codec-adapter table, the raw-YUV scorer | ADR-1124 |
| Stage 5b | `conformal` CLI wiring, shipped as `predict --with-uncertainty` and `recommend --with-uncertainty` | ADR-0393 |
| Stage 6 | `fast` subcommand, `pkg/fast`, `pkg/conformal`, `pkg/scorebackend`. The smoke path is complete; the production path is blocked on ONNX named inputs ([Production-mode blocker](#production-mode-blocker-onnx-named-inputs)). | ADR-0276 / ADR-0304 |
| Stage N | Feature parity, then rename the binary to `vmafx-tune` | Planned |

`tune-per-shot --fast-nr` and a production-mode `fast` remain open and wait on
an ONNX Go binding.

### Roadmap table corrections

An earlier revision of the roadmap listed `pkg/conformal` as merged under
Stage 3. It was not: no such package existed on `master`. The package landed
with Stage 6, and the roadmap row was corrected. The `pkg/tune/{predictor,codec,
pyjson}` aliases were first kept for the in-flight sidecar parity fix
([#1187](https://github.com/VMAFx/vmafx/pull/1187)); that fix has landed, and
the aliases are still imported by the sidecar.

## See also

- [vmaf-tune.md](vmaf-tune.md): overview of the tuning workflow and the Python
  CLI.
- [vmaf-tune-corpus.md](vmaf-tune-corpus.md): corpus JSONL schema and flags.
- [vmaf-tune-recommend.md](vmaf-tune-recommend.md) and
  [vmaf-tune-coarse-to-fine.md](vmaf-tune-coarse-to-fine.md): the CRF search.
- [vmaf-tune-fast-path.md](vmaf-tune-fast-path.md): the proxy fast path.
- [vmaf-tune-codec-adapters.md](vmaf-tune-codec-adapters.md),
  [vmaf-tune-hdr-and-sampling.md](vmaf-tune-hdr-and-sampling.md),
  [vmaf-tune-score-backend.md](vmaf-tune-score-backend.md): adapters, HDR and
  scoring backends.
- [vmafx-ort-runner.md](vmafx-ort-runner.md) and
  [vmaf-perShot.md](vmaf-perShot.md): the two helper binaries.
