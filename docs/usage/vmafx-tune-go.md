<!-- markdownlint-disable MD013 MD060 -->
# vmafx-tune-go — Go port of vmaf-tune (Stage 5)

`vmafx-tune-go` is the Go port of the `vmaf-tune` rate-quality tuning CLI
(Stages 1–5). It ships as a **separate binary** alongside the Python `vmaf-tune`
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

### `auto` — Phase F adaptive recipe-aware planner

Composes the per-phase tuning stages into one deterministic decision tree and
emits a JSON plan. Optionally realises the winning cell as a real encode plus a
libvmaf score.

```text
vmafx-tune-go auto --src <video> [flags]
```

**Required flags:**

| Flag | Description |
|------|-------------|
| `--src` | Reference video (raw YUV or any FFmpeg-readable container) |

**Optional flags:**

| Flag | Default | Description |
|------|---------|-------------|
| `--target-vmaf` | `93` | Target pooled-mean VMAF. |
| `--max-budget-bitrate` | `8000` | Upper bound on the picked rendition's bitrate, in kbps. |
| `--allow-codecs` | `libx264` | Comma-separated codec list the tree may pick from. A single entry short-circuits the compare-shortlist stage. |
| `--codec` | *(unset)* | Pin the codec choice, overriding the `--allow-codecs` ranking. Also short-circuits the shortlist stage. |
| `--sample-clip-seconds` | `0` | Propagate this clip length to internal sweeps rather than re-deciding per stage. `0` = full source. |
| `--smoke` | `false` | Exercise the composition with synthetic metadata — no ffprobe, no ffmpeg, no ONNX. |
| `--output` | stdout | Write the JSON plan here. |
| `--execute` | `false` | After planning, run real FFmpeg encodes and libvmaf scores for the selected cell(s). |
| `--runs-dir` | `runs` | Output directory for encoded files and `tune_results.jsonl` (used with `--execute`). |
| `--execute-all` | `false` | With `--execute`: run every plan cell, not just the winner. Useful for post-hoc A/B comparison. |
| `--model` | *(unset)* | Optional `predictor_<codec>.onnx` path. Default uses the analytical fallback curve. |

`--model` is a Go-side addition: the Python `auto` driver always constructs its
predictor without a model path, which is the analytical fallback this flag
defaults to. Supplying a model routes inference through the ONNX bridge in
`pkg/ai`, which degrades back to the analytical curve when the ORT runner is
not on `PATH`.

**Example — plan only:**

```bash
vmafx-tune-go auto \
  --src src.mp4 \
  --target-vmaf 93 \
  --allow-codecs libx264,libx265
```

**Example — plan and realise the winner:**

```bash
vmafx-tune-go auto \
  --src src.mp4 \
  --target-vmaf 95 \
  --max-budget-bitrate 6000 \
  --execute --runs-dir runs/
```

#### How the plan is built

1. **Probe** the source: geometry and duration via `ffprobe`, HDR signalling via
   the colour-metadata classifier. Every probe failure degrades to conservative
   defaults (1920x1080, duration 0, SDR) rather than aborting the run.
2. **Apply the content recipe.** The content class selects a small override set
   — a narrower or wider conformal-confidence gate, a forced single-rung ladder,
   a saliency intensity, and a target-VMAF offset. An HDR source carrying only a
   generic `live_action` label is promoted to the HDR recipe. The overrides load
   from `ai/data/phase_f_recipes_calibrated.json` when that file is reachable,
   and fall back to the documented placeholders with a one-line warning
   otherwise.
3. **Walk the ten short-circuits**, recording each one that fires under
   `metadata.short_circuits`.
4. **Estimate each `(rung, codec)` cell**: invert the predictor for a CRF, then
   estimate the VMAF and bitrate that CRF would produce.
5. **Pick a winner** against the VMAF target and the bitrate budget.

#### Short-circuits

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
| `skip-per-shot` | The source is **both** shorter than 5 minutes **and** below 0.15 shot variance. |
| `low-complexity` | The probe-encode bitrate is under 200 kbps. Dormant when no probe has run. |
| `baseline-meets-target` | A default-CRF encode already meets the target. Dormant when no baseline was scored. |
| `no-two-pass` | The resolved codec adapter does not support two-pass encoding. |

#### Confidence-aware escalation

Each cell carries a conformal interval width, and that width decides whether the
predictor's own verdict is overridden:

| Interval width | Decision |
|----------------|----------|
| `<= tight` (default 2.0) | `skip-escalation` — trust the point estimate even on a `FALL_BACK` verdict. |
| `>= wide` (default 5.0) | `force-escalation` — escalate even on a `GOSPEL` verdict. |
| between | Defer to the native verdict. |
| `NaN` (uncalibrated) | Defer to the native verdict. |

Without a calibration sidecar the interval is uncalibrated, so cells carry `NaN`
and no override happens. The 2.0 / 5.0 defaults are an emergency floor, not a
corpus fit.

#### Plan JSON

The plan is a `{"cells": [...], "metadata": {...}}` object with sorted keys,
byte-compatible with the Python `vmaf-tune auto` output.

Each cell carries: `rung`, `codec`, `verdict`, `crf`, `estimated_vmaf`,
`estimated_bitrate_kbps`, `hdr_args`, `sample_clip_seconds`,
`confidence_decision`, `interval_width`, `effective_predictor_target_vmaf`,
`prediction_source`, `saliency_intensity`, and `selected`.

`metadata.winner.status` is one of:

| Status | Meaning |
|--------|---------|
| `budget_and_quality_met` | A cell satisfies both the target and the budget. |
| `quality_met_budget_exceeded` | Quality is reachable, but every such cell is over budget; the smallest overage wins. |
| `target_unmet` | No cell reaches the target; the closest miss is returned so you get a concrete next encode. |
| `no_eligible_cells` | No cell carried finite estimates. |

> **The plan JSON is not strict RFC 8259.** An uncalibrated `interval_width` is
> emitted as the bare token `NaN`, exactly as CPython's `json.dumps` does with
> its default `allow_nan=True`. This is deliberate byte-compatibility with the
> Python emitter. Parse it with Python's `json` module or another permissive
> parser; `jq --strict` and Go's `encoding/json` will reject it. The
> `--execute` results log (`tune_results.jsonl`) *is* strict — non-finite
> values there are rendered as `null`.

#### Execute mode

With `--execute` the selected cell is encoded and scored, and one row per
executed cell is **appended** to `<runs-dir>/tune_results.jsonl`. Encoded files
land beside it as `encode_<index>_<codec>_<preset>_crf<n>.mkv`.

A failed encode is recorded in its row with a non-zero `encode_exit_status`, and
scoring is skipped for that cell. The command exits non-zero only when cells
were executed and **none** scored successfully.

### `sidecar` — Local on-host predictor sidecar

Trains and inspects a bias-correction term on top of the shipped predictor:

```text
sidecar_vmaf = predictor_vmaf + sidecar_correction(features)
```

The shipped predictor is never mutated, so model upgrades stay deterministic and
reproducible across hosts.

```text
vmafx-tune-go sidecar <status|predict|record|batch-record> [flags]
```

**Flags shared by every nested subcommand:**

| Flag | Default | Description |
|------|---------|-------------|
| `--codec` | `libx264` | Codec bucket for the sidecar state. |
| `--cache-dir` | `${XDG_CACHE_HOME:-~/.cache}/vmaf-tune/sidecar` | Sidecar cache root. |
| `--predictor-version` | `predictor_v1` | Predictor-version namespace. |
| `--model` | *(unset)* | Optional `predictor_<codec>.onnx` path; default uses the analytical fallback. |
| `--json` | `false` | Emit machine-readable JSON instead of the one-line text form. |

**Nested subcommands:**

| Subcommand | Extra flags | Purpose |
|------------|-------------|---------|
| `status` | — | Print state metadata: codec, host UUID, state path, predictor version, update count, residual RMS. |
| `predict` | `--features-json`, `--crf` | Predict VMAF with the correction folded in. Reports the base score, the correction, and the sum. |
| `record` | `--features-json`, `--crf`, `--observed-vmaf`, `--no-persist` | Fold one observed encode result into the fit. |
| `batch-record` | `--captures-jsonl` | Fold a JSONL capture file, one observation per row, persisting once at the end. |

**Example — inspect, train from a capture log, then predict:**

```bash
vmafx-tune-go sidecar status --json
vmafx-tune-go sidecar batch-record --captures-jsonl captures.jsonl --json
vmafx-tune-go sidecar predict --features-json shot.json --crf 26 --json
```

#### Feature JSON

`--features-json` takes an object of shot features, or a `{"features": {...}}`
wrapper so a capture row can carry `crf` and `observed_vmaf` alongside. Four keys
are required; every other field defaults to `0`.

| Key | Required | Meaning |
|-----|----------|---------|
| `probe_bitrate_kbps` | yes | Average bitrate over the probe encode. |
| `probe_i_frame_avg_bytes` | yes | Mean I-frame size. |
| `probe_p_frame_avg_bytes` | yes | Mean P-frame size. |
| `probe_b_frame_avg_bytes` | yes | Mean B-frame size (0 for codecs without B-frames). |
| `saliency_mean`, `saliency_var` | no | Saliency signals; 0 when unavailable. |
| `frame_diff_mean`, `y_avg`, `y_var` | no | FFmpeg `signalstats` aggregates. |
| `shot_length_frames`, `fps`, `width`, `height` | no | Structural metadata. |

`batch-record` reads the same object per line, plus `crf` and `observed_vmaf`. A
malformed row is reported on `stderr` and skipped; the rest of the file still
lands. That is deliberate — a capture log is often partially corrupt after an
interrupted run, and losing the good rows to one bad line would be worse.

#### State and privacy

State lives at:

```text
${XDG_CACHE_HOME:-~/.cache}/vmaf-tune/sidecar/
  host-uuid                                    # random 128-bit token
  <predictor-version>/<codec>/state.json       # ridge weights + inverse Gram
```

The host UUID is drawn from a CSPRNG on first use. It is **never** derived from a
MAC address, hostname, `/etc/machine-id`, CPUID, or any other
machine-identifying signal.

A predictor-version or schema mismatch on load discards the fit and resets to
cold start, keeping only the host UUID. That is what makes a shipped-model
upgrade safe: a stale correction can never be replayed against a refreshed
predictor. At cold start the weights are zero, so the correction is exactly `0.0`
and the sidecar returns the bare predictor's value untouched.

A corrupt `state.json` also cold-starts, and the corrupt file is left in place so
you can inspect it.

## Not yet ported (Stage 6+)

The following subcommands are stubs in `vmafx-tune-go`. They log a redirect
notice (a `WARN`-level structured log line) and exit 1 when invoked. Use the
Python `vmaf-tune` binary for these:

| Subcommand | Python equivalent |
|------------|-------------------|
| `tune-per-shot` | `vmaf-tune tune-per-shot` |
| `fast` | `vmaf-tune fast` |
| `corpus` | `vmaf-tune corpus` |
| `benchmark` | `vmaf-tune benchmark` |
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
| Stage 5 | `auto` (Phase F planner + execute mode) and `sidecar` subcommands | ADR-0705 / ADR-0730 | **This PR** |
| Stage 6 | `tune-per-shot` subcommand, conformal CLI wiring | Planned | — |
| Stage 7 | `fast` subcommand (requires ONNX Go binding) | Planned | — |
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

Stage 5 adds the `auto` / `sidecar` stack under `pkg/tune/`:

- **`pkg/tune/auto/`** — the Phase F decision tree: source probing, the ten
  short-circuit predicates, the recipe table, the confidence policy, winner
  selection, and the plan emitter.
- **`pkg/tune/sidecar/`** — the online-ridge bias-correction model, its
  Sherman-Morrison rank-1 update, and the cache-dir persistence layout.
- **`pkg/tune/predictor/`** — `ShotFeatures`, the per-codec analytical curve,
  the optional ONNX path, and the `PickCRF` binary-search inversion.
- **`pkg/tune/codec/`** — the codec-adapter registry: quality windows, probe
  knobs, preset vocabularies, and per-encoder ffmpeg argv.
- **`pkg/tune/hdr/`** — HDR detection from ffprobe colour metadata plus the
  per-codec HDR flag dispatch.
- **`pkg/tune/executor/`** — `--execute` mode: ffmpeg argv construction, the
  libvmaf CLI driver, and the JSONL results log.
- **`pkg/tune/pyjson/`** — a CPython-compatible JSON emitter. Reproduces
  `json.dumps(obj, indent=N, sort_keys=True)` byte for byte, including the
  `NaN` / `Infinity` tokens and CPython's `repr()` float spelling.
- **`pkg/tune/pymath/`** — correctly-rounded `Exp2` and `Log10`. Go's
  `math.Pow` and `math.Log10` land a ULP away from the platform libm CPython
  uses, which is enough to move the last mantissa digit of a JSON field; these
  kernels close that gap. The package docs record the measured residual.

See [ADR-0705](../adr/0705-vmafx-tune-go-stage1.md) for the migration rationale,
[ADR-0730](../adr/0730-vmafx-tune-go-stage2.md) for Stage-2,
[ADR-0770](../adr/0770-vmafx-tune-go-stage4-report.md) for Stage-4, and
[ADR-0702](../adr/0702-vmafx-phase4-language-modernization.md) for the Phase 4 umbrella.
