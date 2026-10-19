<!-- markdownlint-disable MD060 -->
# `vmaf-tune compare` — codec comparison

`vmaf-tune compare` answers "should I migrate from x264 to SVT-AV1 yet?" per
source. Given one reference and a target VMAF, it runs each codec's
target-VMAF bisect in parallel and ranks the results by smallest file. It is
Bucket #7 of the [`vmaf-tune` capability
audit](../research/0061-vmaf-tune-capability-audit.md);
the default backend is the target-VMAF bisect of
[ADR-0326](../adr/0326-vmaf-tune-phase-b-bisect.md). The tool overview is in
[`vmaf-tune.md`](vmaf-tune.md).

## Quick start

```shell
vmaf-tune compare \
    --src ref.yuv \
    --width 1920 --height 1080 --pix-fmt yuv420p \
    --framerate 24 --duration 10 \
    --sample-clip-seconds 4 \
    --target-vmaf 92 \
    --encoders libx264,libx265,libsvtav1,libaom-av1,libvvenc \
    --crf-min 15 --crf-max 40 \
    --format markdown
```

Abridged output (`--format markdown`):

```markdown
# Codec comparison — target VMAF 92

- Source: `ref.yuv`
- Tool: `vmaf-tune <version>`
- Wall time: 6421.3 ms

| Rank | Codec    | Encoder         | Best CRF | Bitrate (kbps) | Encode time (ms) | VMAF  | Status |
|---:|---|---|---:|---:|---:|---:|---|
| 1  | libaom-av1 | libaom-3.8.0   | 30       |         1500.0 |          18000.0 | 92.40 | ok     |
| 2  | libx265   | libx265-3.5     | 26       |         1700.0 |           4200.0 | 92.00 | ok     |
| 3  | libsvtav1 | libsvtav1-1.7.0 | 32       |         1900.0 |           2800.0 | 92.30 | ok     |
| 4  | libx264   | libx264-164     | 23       |         2400.0 |           1500.0 | 92.10 | ok     |

**Smallest file**: `libaom-av1` at CRF 30 → 1500.0 kbps (VMAF 92.40).
```

Exit codes: `0` when at least one codec row succeeded, `1` when none did, and
`2`
for an argument error or an unavailable score backend.

## How a run works

- **Source geometry.** Raw YUV does not describe itself, so the real bisect
  backend needs `--width` and `--height`. Set `--pix-fmt`, `--framerate` and
  `--duration` too: they default to the common SDR 24 fps shape, and wrong
  values skew scoring and bitrate math.
- **Sample window.** `--sample-clip-seconds N` evaluates the centre `N`
  seconds of the source on every bisect iteration. Compare forwards matching
  `--frame_skip_ref` / `--frame_cnt` bounds to the scorer and normalises
  bitrate against the sample duration.
- **Custom rankers.** `--predicate-module MODULE:CALLABLE` accepts any
  importable `(codec, src, target_vmaf) -> RecommendResult` callable and
  bypasses the bisect backend. It is meant for custom rankers and tests.
- **Hardware encoders.** An encoder that is missing or has no compatible GPU
  yields a row that is skipped with a reason. It does not fail the whole run.
- **Search window.** The bisect starts from the encoder's absolute CRF range,
  which makes VMAF 95 and above reachable. The full contract is in
  [`vmaf-tune-bisect.md`](vmaf-tune-bisect.md).

### Container sources auto-probe their framerate

When `--src` is a container (mp4, mkv, mov, Y4M, ...) and you leave
`--framerate` and `--duration` at their defaults, the CLI calls `ffprobe` and
substitutes the probed values before the bisect starts (ADR-0509). This
avoids a silent failure: the `24` default against a 60 fps source misaligns
the reference and distorted decodes and collapses VMAF into the 4 to 90 band
regardless of CRF.

- An explicit `--framerate` or `--duration` still wins.
- A stderr warning fires when an explicit value disagrees with the probed rate
  (subsampling is legitimate, but it should be deliberate).
- Raw `.yuv` sources are not probed, because raw YUV does not self-describe.

## Flags

| Flag | Default | Meaning |
|---|---|---|
| `--src PATH` | none (required) | Single reference clip. |
| `--target-vmaf F` | `92.0` | Single VMAF target. Passed explicitly with `--target-vmafs` left at its default, it selects the single-target v1 schema. |
| `--target-vmafs LIST` | `94,96,97,98` | Comma-separated targets to sweep per codec (ADR-0538). One value selects the v1 schema; two or more select v2. |
| `--encoders LIST` | `libx265,libsvtav1` | Comma-separated encoders. Accepts any registered adapter, including hardware ones such as `h264_nvenc`, `av1_qsv` and `hevc_amf`, and `ADAPTER@VARIANT` tokens. |
| `--encoder-ffmpeg-bin ENCODER=PATH` | off | Repeatable. Binds one compare token to a specific FFmpeg binary, for example `libsvtav1@svt-av1-hdr=/opt/ffmpeg-8.1.1-svtav1-hdr/bin/ffmpeg`. Unbound tokens use `--ffmpeg-bin`. |
| `--width`, `--height` | none | Source geometry. Required for the real bisect backend. |
| `--pix-fmt` | `yuv420p` | Source pixel format forwarded to the scorer. |
| `--framerate` | `24.0` (auto-probed for containers) | Source framerate, see [auto-probe](#container-sources-auto-probe-their-framerate). |
| `--duration` | `0.0` (auto-probed for containers) | Source duration in seconds, used for bitrate math. |
| `--sample-clip-seconds` | `0.0` | `0.0` scores the full source. A positive value shorter than `--duration` uses a centre window for encode, score and bitrate math (ADR-0301). |
| `--preset` | `medium` | Preset forwarded to every codec adapter, which maps it onto its own preset vocabulary. |
| `--crf-min`, `--crf-max` | encoder absolute range | Inclusive CRF search window. Pass both or neither. |
| `--max-iterations` | `8` | Encode and score round-trip cap per codec. |
| `--vmaf-model` | `vmaf_v1.0.16_3d0h` | VMAF model forwarded to the scorer. |
| `--neg` | off | Use the VMAF NEG model variant. |
| `--fast-nr` | off | NR early-elimination inside each bisect, see [`vmaf-tune-fast-nr.md`](vmaf-tune-fast-nr.md). |
| `--score-backend` | unset | `cpu`, `cuda`, `sycl`, `hip`, `metal` or `auto`. See [score backends](vmaf-tune-score-backend.md). |
| `--ffmpeg-bin`, `--vmaf-bin` | `ffmpeg`, `vmaf` | Binary overrides. |
| `--vaapi-device PATH` | auto | VA-API render node for Intel QSV device initialisation, for example `/dev/dri/renderD129`, used by the availability probe and by every QSV encode of the run. `auto` takes the first Intel render node from `/sys/class/drm`. The `VMAFTUNE_VAAPI_DEVICE` variable also works, and the flag wins. |
| `--format` | `markdown` | `markdown`, `json`, `csv`, `html` or `both`. `html` and `both` render the profile card directly. `both` writes `.json`, `.html` and `.md` next to `--output` and therefore requires it. |
| `--json-sidecar` | off | With a single HTML or Markdown report, also write `<output>.json` with the report payload. `--format both` includes it already. Requires `--output`. |
| `--output PATH` | stdout | Write the report to PATH. |
| `--no-parallel` | off | Run codecs one after another. The default is a thread pool with one worker per codec. |
| `--max-workers N` | number of encoders | Cap on the thread pool size. |
| `--predicate-module MOD:FN` | off | Advanced hook that bypasses the bisect backend. |
| `--no-bisect` | off | CRF sweep mode, see [CRF sweep mode](#crf-sweep-mode). |
| `--crf-sweep LIST` | none | CRF values for `--no-bisect`, for example `18,23,28,33`. Required with `--no-bisect`. |
| `--workdir PATH` | none | Parent of the per-run scratch directory that holds the decoded reference YUV and the encodes. Falls back to `VMAFTUNE_WORKDIR`, then the OS default (`/tmp`). See [workdir](#workdir-and-decode-concurrency). |
| `--max-concurrent-decodes N` | `1` | Cap on simultaneous reference-YUV decodes across all codec threads. See [workdir](#workdir-and-decode-concurrency). |

### Workdir and decode concurrency

`--workdir` matters when the source is large and `/tmp` is a small `tmpfs`: a
634-second 1080p60 source decodes to about 118 GB of raw YUV
([ADR-0598](../adr/0598-vmaftune-workdir-relocation.md)).

`--max-concurrent-decodes` bounds the peak disk use
([ADR-0577](../adr/0577-vmaftune-bisect-concurrency-cap-and-aggressive-cleanup.md)).
At the default of `1`, decodes are serial and the peak stays at one YUV
regardless of how many codecs run. With three codecs and a 110 GB source,
that is 110 GB instead of the 330 GB peak that caused an ENOSPC failure.
Raise `N` only when `--workdir` sits on a volume with enough free space and
I/O for `N` parallel decode streams. Encoder runs are always parallel; only
the decode-to-raw-YUV step is serialised.

## Multi-target rate-quality sweep

`compare` defaults to a 4-point sweep, `--target-vmafs 94,96,97,98`
(ADR-0516, ADR-0534, ADR-0538). The JSON output stamps `schema_version: 2`
and has one row per `(codec, target_vmaf)` pair. Each row can also carry a
`bisect_samples` list with every encode and score probe the bisect computed.

`vmaf-tune report --compare-json sweep.json` renders a per-codec rate-quality
curve from those probes. Picked-CRF rows appear as larger circled markers, and
the Pareto frontier (lowest bitrate at each target) is a heavier dashed
overlay.

```shell
# Defaults: 4 targets, two encoders in this example, GPU scoring.
vmaf-tune compare \
    --src bbb_1080p_60fps.mp4 \
    --width 1920 --height 1080 --framerate 60 \
    --encoders libx265,libsvtav1 \
    --sample-clip-seconds 3 --max-iterations 3 \
    --score-backend cuda \
    --format json --output sweep.json
vmaf-tune report \
    --src bbb_1080p_60fps.mp4 \
    --compare-json sweep.json --target-vmaf 92 \
    --format both --output sweep_report.html
```

### Profile cards straight from compare

`compare` can render the same profile card directly, and `encode-profile` can
then reuse one recommendation without re-running the sweep:

```shell
vmaf-tune compare \
    --src bbb_1080p_60fps.mp4 \
    --width 1920 --height 1080 --framerate 60 \
    --encoders libx265,libsvtav1,av1_nvenc,av1_qsv \
    --sample-clip-seconds 3 --max-iterations 3 \
    --score-backend cuda \
    --format both --output sweep_profile.html

vmaf-tune encode-profile \
    --profile sweep_profile.html \
    --src bbb_1080p_60fps.mp4 \
    --codec libsvtav1 --target-vmaf 96 \
    --output bbb_svtav1_vmaf96.mkv
```

For both `compare` and `report`, `--format both` writes the full three-file
bundle next to `--output`: machine-readable `.json` plus the `.html` and `.md`
renders. On a single-format HTML or Markdown run, `--json-sidecar` adds the
same JSON payload without requesting both renders.

The HTML and Markdown profile cards contain:

- a **Quick takeaways** block before the charts: the smallest successful row
  at each target, the failed or unavailable row count, the ladder span and the
  per-shot CRF spread;
- short "how to read this" notes;
- report-local codec identity chips (text badges linking to the upstream codec
  or vendor project);
- per-codec failure status;
- an embedded `encoder_profile` JSON payload, which `encode-profile` can read
  from the HTML, the Markdown or the raw JSON to build a concrete FFmpeg
  encode. See [`vmaf-tune-report.md`](vmaf-tune-report.md).

### Why these defaults

ADR-0538 supersedes the target defaults of ADR-0534.

- **`94,96,97,98` covers premium-archival operating points.** The fork's
  primary use is archival masters at VMAF 95 and above. VMAF 94 is the
  subjectively transparent floor on 4K source and 98 is the near-lossless
  ceiling. The earlier ADR-0534 default (`75,80,85,90,93`) served
  streaming and broadcast workflows, so its chart held no points that an
  archival user picks CRFs from.
- **VMAF 95 and above is reachable.** The earlier "top stops at 93" limit was
  a bisect-harness artefact: the search window defaulted to the adapter's
  narrow `quality_range`, which the adapter validator also enforced as a hard
  gate. ADR-0538 widens the window to the encoder's absolute CRF range, see
  [`vmaf-tune-bisect.md`](vmaf-tune-bisect.md).
- **The chart renders from `bisect_samples`.** Connecting the picked-CRF rows
  per codec across targets gave physically impossible downward dips, because
  the bisect overshoots each target by a different amount. Plotting every
  probe the bisect already computed shows the real per-codec curve.

## Output schema

The JSON and CSV columns are exported as `vmaftune.compare.COMPARE_ROW_KEYS`:
`codec`, `adapter`, `runtime_variant`, `ffmpeg_bin`, `encoder_version`,
`best_crf`, `bitrate_kbps`, `encode_time_ms`, `vmaf_score`, `target_vmaf`,
`ok` and `error`.

- Failed rows trail successful ones in the ranking.
- A row with `ok=false` carries a human-readable `error`, `-1` for `best_crf`
  and non-finite floats, which JSON writes as `null`.
- `adapter`, `runtime_variant` and `ffmpeg_bin` are provenance fields for
  `ADAPTER@VARIANT` runs. They are empty on rows from old programmatic
  predicates that bind no runtime variant.

Every payload also has the top-level keys `src`, `tool_version`,
`wall_time_ms` and `rows`.

| Schema | Emitted when | Extra top-level keys | Rows |
|---|---|---|---|
| v1 (single target) | One target results: `--target-vmaf` passed explicitly with the default sweep, or `--target-vmafs` with one value. | none (no `schema_version`), plus `target_vmaf` | One per codec. |
| v2 (multi-target sweep) | `--target-vmafs` lists two or more targets (the default). | `schema_version: 2`, `target_vmafs` (for example `[94.0, 96.0, 97.0, 98.0]`) | One per `(codec, target_vmaf)` pair. Each can carry `bisect_samples`. |
| v3 (CRF sweep) | `--no-bisect`. | `schema_version: 3`, `mode: "crf_sweep"`, `crf_sweep`, `target_vmaf` | One per `(codec, crf)` pair. |

### `bisect_samples`

`bisect_samples` is an optional list of `{crf, bitrate_kbps, vmaf_score,
encode_time_ms}` objects, one per encode and score probe of the underlying
bisect (ADR-0534). It is additive and absent on v2 dumps that predate
ADR-0534. The CSV emitter drops it (`extrasaction="ignore"`), so it stays a
JSON-only field.

### How `report` reads the schema

`vmaf-tune report` tells v1 from v2 by the `schema_version` key, or by the
presence of `target_vmafs`.

- **v1** renders the bar-and-dot chart.
- **v2 with `bisect_samples`** plots every probe per codec, deduplicated by
  CRF and sorted by bitrate. It overlays the picked-CRF rows as larger circled
  markers and keeps the Pareto frontier as the dashed overlay.
- **v2 without `bisect_samples`** (old dumps) falls back to the legacy
  connect-the-dots line, with a caveat note in the title. That line can show
  physically impossible dips when the per-target overshoot varies, which is
  the failure that ADR-0534 fixes for the new path.

!!! note "Encode time is wall clock"
    `encode_time_ms` is wall clock on whatever machine ran the predicate.
    Cross-codec time comparisons only make sense when every predicate ran on
    the same hardware in the same configuration, see
    [Research-0061 Bucket #7](../research/0061-vmaf-tune-capability-audit.md).

## CRF sweep mode

With `--no-bisect` and `--crf-sweep`, `compare` skips the target-VMAF bisect
and encodes each `(codec, CRF)` pair exactly once. Use it to sweep a fixed CRF
ladder such as 18, 23, 28, 33 and inspect the resulting bitrate and VMAF
pairs. It is faster than four separate bisect sweeps and more direct than the
`corpus` pipeline.

```shell
vmaf-tune compare \
    --src clip.mp4 \
    --encoders libx264,libx265,libsvtav1 \
    --no-bisect --crf-sweep 18,23,28,33 \
    --duration 60 --sample-clip-seconds 30 \
    --format json --output cmp_sweep.json
```

Three codecs times four CRFs give 12 rows in `cmp_sweep.json`.

- **Schema.** The output is v3 (ADR-0548): `schema_version: 3`,
  `mode: "crf_sweep"`, `crf_sweep: [18, 23, 28, 33]`. Each row carries `codec`,
  `adapter`, `runtime_variant`, `ffmpeg_bin`, `crf`, `bitrate_kbps`,
  `vmaf_score`, `encode_time_ms`, `encoder_version`, `ok` and `error`.
- **Targets.** `--target-vmaf` and `--target-vmafs` are accepted but do not
  drive the encode loop; the payload only records `target_vmaf`.
- **Format.** The mode always writes JSON. `--format html` and `--format both`
  exit with code `2`; emit JSON and pass it to `vmaf-tune report` instead.
- **Availability.** Hardware encoders are probed as on the bisect path. An
  unavailable encoder (for example `h264_nvenc` without an NVIDIA device)
  produces `ok=false` rows for each CRF and does not abort the run.

## See also

- [`vmaf-tune.md`](vmaf-tune.md) — the tool overview.
- [`vmaf-tune-bisect.md`](vmaf-tune-bisect.md) — the bisect that `compare`
  runs per codec, including the high-VMAF contract (ADR-0538).
- [`vmaf-tune-report.md`](vmaf-tune-report.md) — `report` and `encode-profile`.
- [`vmaf-tune-fast-nr.md`](vmaf-tune-fast-nr.md) — `--fast-nr`.
- [`vmaf-tune-codec-adapters.md`](vmaf-tune-codec-adapters.md) — adapters and
  `ADAPTER@VARIANT` tokens.
- [`vmaf-tune-score-backend.md`](vmaf-tune-score-backend.md) —
  `--score-backend`.
- [`vmaf-tune-workdir.md`](vmaf-tune-workdir.md) — scratch space and
  `VMAFTUNE_WORKDIR`.
- [Research-0061](../research/0061-vmaf-tune-capability-audit.md) —
  capability audit.
