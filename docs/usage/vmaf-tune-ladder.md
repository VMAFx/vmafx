<!-- markdownlint-disable MD013 MD060 -->
# `vmaf-tune ladder` — per-title ABR ladder construction

`vmaf-tune ladder` builds a per-title adaptive-bitrate ladder for one source:
it samples the (resolution, target-VMAF) plane, keeps the Pareto hull of the
(bitrate, VMAF) points, picks `--quality-tiers` rungs along the hull and writes
an HLS master playlist, a DASH MPD or a JSON descriptor.

This is the
"[per-title
encoding](https://netflixtechblog.com/per-title-encode-optimization-7e99442b62a2)"
loop in one command: the ladder that is optimal for this title replaces a fixed
authoring-spec ladder.
[ADR-0295](../adr/0295-vmaf-tune-phase-e-bitrate-ladder.md)
records the design and the alternatives (geometric ladder, JND-spaced ladder,
fixed Apple HLS ladder).

## Quick start

A five-rung 1080p-to-240p ladder against five VMAF targets:

```shell
vmaf-tune ladder \
    --src episode01.yuv \
    --encoder libx264 \
    --resolutions 1920x1080,1280x720,854x480,640x360,426x240 \
    --target-vmafs 95,90,85,75,65 \
    --quality-tiers 5 \
    --framerate 24 --duration 10 \
    --format hls \
    --output episode01_ladder.m3u8
```

`--resolutions` and `--target-vmafs` are required and have no defaults; the
lists above are an example, not a built-in ladder. Add `--framerate` and
`--duration` that match the source, because the bitrate in the manifest is
`encode size / duration` and `--duration` defaults to `1.0` second.

The output is an HLS master playlist with one `#EXT-X-STREAM-INF` per rung,
bandwidth (in bps) increasing from rung to rung. Variant URIs are placeholders
of the form `rendition_<W>x<H>_<kbps>k.m3u8`; re-point them at your
per-rendition playlists when you package the encoded segments.

!!! note "CODECS attribute"
    The HLS `CODECS` and DASH `codecs` attributes name each rung's codec,
    profile and level (RFC 6381). They come from a two-frame encode with the
    ladder's encoder and preset at the rung's geometry and frame rate,
    read from the codec configuration record of the result: `avc1.PPCCLL`
    for H.264, `hvc1.…` for HEVC (package those renditions with
    `-tag:v hvc1`), `av01.P.LLT.DD` for AV1 and `vp09.PP.LL.DD` for VP9
    (level from the VP9 level table). An encoder without such a record
    (VVC, ProRes) makes HLS and DASH output fail with exit status 2; use
    `--format json`. The writers used to print `avc1.640028` for every
    encoder.

## Pipeline

1. `build_ladder()` samples every `(resolution, target_vmaf)` pair.
2. The default sampler encodes and scores a CRF sweep at that resolution
   (default `20,25,30,35,40`) and keeps the lowest-bitrate row that meets the
   target. [Details](vmaf-tune-ladder-default-sampler.md).
3. `convex_hull()` drops dominated points (a Pareto filter) and then keeps the
   upper-convex envelope, so both bitrate and VMAF rise strictly along it.
4. `select_knees()` picks the requested number of rungs along the hull; the
   first and last hull points are always kept.
5. `emit_manifest()` writes HLS, DASH or JSON.

Programmatic callers can pass `sampler=` to `build_ladder()` for a finer grid,
a bisect-backed loop or a precomputed corpus stream.

## Flags

| Flag | Default | Meaning |
| --- | --- | --- |
| `--src PATH` | required | Source: raw `.yuv`, `.y4m` or any FFmpeg-readable container. |
| `--encoder NAME` | `libx264` | Any registered codec adapter. |
| `--resolutions WxH,...` | required | Comma-separated rung resolutions. |
| `--target-vmafs F,...` | required | Comma-separated VMAF targets, sampled at every resolution. |
| `--quality-tiers N` | `5` | Rungs to pick from the hull. |
| `--format` | `hls` | `hls`, `dash` or `json`. |
| `--spacing` | `log_bitrate` | `log_bitrate`, `vmaf`, or `uniform` (legacy alias of `vmaf`). |
| `--output PATH` | stdout | Manifest destination; parent directories are created. |
| `--framerate F` | `24.0` | Source frame rate for the default sampler. Match the real source. |
| `--duration S` | `1.0` | Analysed window in seconds. Sets the bitrate divisor and bounds the encode and the reference decode. |
| `--pix-fmt FMT` | `yuv420p` | Source pixel format, for example `yuv422p` or `yuv420p10le`. |
| `--crf-sweep CSV` | sampler default `20,25,30,35,40` | CRF list that replaces the default sweep. |
| `--src-width INT`, `--src-height INT` | largest `--resolutions` entry (by pixel count), each independently | Actual raw-YUV source size when it exceeds a rung. Container sources auto-detect geometry and ignore both. |
| `--score-backend NAME` | `auto` | `auto`, `cpu`, `cuda`, `sycl`, `hip` or `metal` (Vulkan was removed in ADR-0726). |
| `--vmaf-bin PATH` | `vmaf` | `vmaf` binary used to probe the scoring backends. |
| `--neg` | off | Use the NEG variant of the VMAF model (see [VMAF NEG](../metrics/vmaf-neg.md)). |
| `--with-uncertainty` | off | Prune and insert rungs from conformal intervals. See [below](#uncertainty-aware-extension). |
| `--uncertainty-sidecar PATH` | built-in thresholds | Calibration sidecar JSON for the recipe. |
| `--rung-overlap-threshold F` | `0.5` | Interval-overlap fraction above which the lower-bitrate rung of a pair is dropped. |
| `--workdir PATH` | `VMAFTUNE_WORKDIR`, else the system temporary directory | Parent of each rung's scratch directory: the raw-YUV reference decode and the encodes ([workdir](vmaf-tune-workdir.md)). |
| `--max-concurrent-decodes N` | `1` | Cap on reference decodes in flight ([ADR-0577](../adr/0577-vmaftune-bisect-concurrency-cap-and-aggressive-cleanup.md)). The ladder samples one rung at a time, so it never runs more than one decode; the cap matters only for callers that share the semaphore. |

`--score-backend auto` prefers `cuda`, then `sycl`, `hip`, `cpu`. A named
backend is honoured strictly: the run exits with code 2 before any encode when
the local `vmaf` binary does not offer it, so use `cpu` to force bit-exact
scoring against the Netflix golden gate. The resolved backend is printed on
stderr (`vmaf-tune ladder: scoring backend = ...`).

## Source files

`--src` accepts any input FFmpeg can decode. The ladder, corpus and bisect
paths decode the reference once per sweep into a `.ref.decoded.yuv` sidecar
under the scratch directory and reuse it for every cell, so there is no need to
pre-decode the source by hand
([ADR-0499](../adr/0499-vmaf-tune-ladder-reference-decode-v3.md)).

- **Raw `.yuv` (or no suffix).** Used as is. Give the real size with
  `--src-width` and `--src-height` when it exceeds a rung; see
  [Cross-resolution rungs](#cross-resolution-rungs).
- **Anything else, including `.y4m` and `.mp4`, `.mkv`, `.webm`.** Treated as a
  container: FFmpeg detects the format and geometry, and the reference is
  decoded to raw YUV because `vmaf-tune` always passes explicit geometry flags
  to libvmaf, which then expects raw planar input.

When `--duration` is set, the reference decode is clamped to that window, so a
10-second probe of a multi-minute source produces a bounded YUV instead of
the whole file.

### Cross-resolution rungs

A rung below the source resolution is encoded through a per-rung
`-vf scale=W:H` filter. The reference leg is downscaled to the same rung target,
so libvmaf reads both legs at one geometry. The per-rung reference sidecar is
named `<src>.ref.decoded.<W>x<H>.yuv`, so a multi-rung sweep in one scratch
directory does not collide. A single-resolution ladder, or a rung that matches
the source, keeps the plain decode with no scaling.

For a raw source whose size differs from a rung, `--src-width` and
`--src-height` become the demuxer-side `-s W:H`. They default to the largest
`--resolutions` entry, so `--resolutions 1920x1080,1280x720,854x480` against a
1080p raw YUV needs neither flag.

## Manifest formats

```shell
# HLS master playlist (default)
vmaf-tune ladder --src ep01.yuv --resolutions 1920x1080,1280x720 \
    --target-vmafs 95,90 --format hls --output ladder.m3u8

# DASH MPD
vmaf-tune ladder --src ep01.yuv --resolutions 1920x1080,1280x720 \
    --target-vmafs 95,90 --format dash --output ladder.mpd

# JSON descriptor (machine-readable, vmaf-tune-ladder/v1 schema)
vmaf-tune ladder --src ep01.yuv --resolutions 1920x1080,1280x720 \
    --target-vmafs 95,90 --format json --output ladder.json
```

The JSON descriptor has three top-level fields:

| Field | Content |
| --- | --- |
| `schema` | Schema identifier, `"vmaf-tune-ladder/v1"`. |
| `renditions[]` | The rungs `select_knees` picked, ascending by bitrate. Each entry has `width`, `height`, `bitrate_kbps`, `bandwidth_bps`, `vmaf` and `crf`. |
| `samples[]` | Every `(resolution, crf)` row the sampler scored, before the hull. Ascending by `(pixel_count, bitrate_kbps)`, same entry shape, de-duplicated by `(width, height, crf)`. Always present: an empty list when no cell was scored. |

`vmaf-tune report --ladder-json` reads `samples[]` to draw the Pareto-cloud
overlay ([`vmaf-tune-report.md`](vmaf-tune-report.md)).

## Rung spacing

`--spacing` sets the coordinate in which `select_knees` spaces the rungs
between the first and last hull point, snapping each ideal position to the
nearest hull point:

| Value | Spacing |
| --- | --- |
| `log_bitrate` (default) | Equal steps in log bitrate, the Apple HLS authoring convention. Bandwidth grows by a constant factor per rung. |
| `vmaf` | Equal VMAF gaps, which follows how viewers perceive quality steps. |
| `uniform` | Legacy alias of `vmaf`. |

## Uncertainty-aware extension

`--with-uncertainty` ([ADR-0393](../adr/0393-fr-regressor-v2-probabilistic.md),
built on the conformal-VQA surface of PR #488) applies the same interval-based
recipe as [`recommend`](vmaf-tune-recommend.md) before knee selection. It is
wired through the library API and the CLI, and nothing changes when the flag is
omitted.

The default sampler keeps the `vmaf_interval` block of each corpus row. When
intervals are present they drive the recipe. Point-only rows are widened to a
centred interval of width `wide_interval_min_width`, which keeps the recipe
conservative: pruning still has overlap data, and wide gaps between rungs can
still receive a mid-rung.

### Why it helps

A point-estimate ladder treats every rung's VMAF as exact. The predictor's
intervals bear directly on rung choice:

- **Adjacent rungs with strongly overlapping intervals.** The predictor cannot
  tell them apart. Ship the higher-quality rung and drop the lower-bitrate one.
- **Adjacent rungs whose averaged interval width is in the WIDE band.** The
  predictor cannot localise the gap. Insert a synthetic mid-rung, the
  highest-information use of the encode budget.

Both transforms run after `convex_hull` and before `select_knees`, so the
Pareto-frontier invariant holds.

### Thresholds

| Setting | Default | Source |
| --- | --- | --- |
| `tight_interval_max_width` | `2.0` VMAF | [Research-0067](../research/0067-vmaf-tune-phase-f-feasibility-2026-05-08.md), shared with the `auto` driver |
| `wide_interval_min_width` | `5.0` VMAF | same |
| `DEFAULT_RUNG_OVERLAP_THRESHOLD` | `0.5` | same |

`--uncertainty-sidecar` uses the schema described on the
[`recommend` page](vmaf-tune-recommend.md); without it, or when it cannot be
read, the defaults apply and a WARNING is logged. The class is
`vmaftune.uncertainty.ConfidenceThresholds` ([conformal
VQA](../ai/conformal-vqa.md)).

### The two transforms

| Transform | Condition | Action |
| --- | --- | --- |
| `prune_redundant_rungs_by_uncertainty` | Overlap of adjacent intervals, divided by the wider width, exceeds `--rung-overlap-threshold` | Drop the lower-bitrate rung. The first and last rungs are always kept, so the bitrate range survives. |
| `insert_extra_rungs_in_high_uncertainty_regions` | Pair-averaged interval width is at least `wide_interval_min_width` | Insert a synthetic mid-rung. |

A synthetic rung takes its fields from the pair `(a, b)`:

- **Bitrate:** geometric midpoint, matching the log-bitrate convention.
- **VMAF:** arithmetic midpoint.
- **Interval:** union of the parent intervals (`min(a.low, b.low)`,
  `max(a.high, b.high)`); conservative on purpose, later encodes refine it.
- **CRF:** rounded average of the parent CRFs.
- **Resolution:** inherited from the higher-quality parent.

`apply_uncertainty_recipe` composes the two in the canonical order: prune
first, so inserted mid-rungs are not immediately pruned against their parents,
then insert.

### Worked example: library

```python
from vmaftune.ladder import (
    UncertaintyLadderPoint,
    apply_uncertainty_recipe,
    convex_hull,
    select_knees,
    emit_manifest,
)
from vmaftune.uncertainty import load_confidence_thresholds

# The sampler emits points with conformal intervals attached.
sampled = [
    UncertaintyLadderPoint(
        width=1920, height=1080, bitrate_kbps=8000.0,
        vmaf=95.5, crf=20, vmaf_low=92.5, vmaf_high=98.5,  # WIDE
    ),
    UncertaintyLadderPoint(
        width=1280, height=720, bitrate_kbps=2500.0,
        vmaf=91.0, crf=24, vmaf_low=88.0, vmaf_high=94.0,  # WIDE
    ),
    UncertaintyLadderPoint(
        width=854, height=480, bitrate_kbps=1200.0,
        vmaf=85.0, crf=27, vmaf_low=84.5, vmaf_high=85.5,  # tight
    ),
]

thresholds = load_confidence_thresholds("calibration.json")
augmented = apply_uncertainty_recipe(sampled, thresholds=thresholds)
# `augmented` may contain synthetic mid-rungs in any wide-interval gap.

hull = convex_hull([p.as_ladder_point() for p in augmented])
rungs = select_knees(hull, n=5, spacing="log_bitrate")
print(emit_manifest(rungs, format="hls"))
```

The 1080p and 720p rungs both have intervals of width 6.0, so their averaged
width is at least `wide_min = 5.0` and the recipe inserts a synthetic rung of
about 4470 kbps and VMAF 93.25 between them. The tight 480p rung is untouched.

### Worked example: CLI

```text
$ vmaf-tune ladder --src trailer.mp4 --encoder libx264 \
    --resolutions 1920x1080,1280x720,854x480 \
    --target-vmafs 95,90,85 --quality-tiers 5 \
    --with-uncertainty --uncertainty-sidecar calibration.json
#EXTM3U
#EXT-X-VERSION:6
#EXT-X-STREAM-INF:BANDWIDTH=1200000,RESOLUTION=854x480,CODECS="avc1.64001e"
rendition_854x480_1200k.m3u8
...
```

With `vmaf_interval` objects in the sampled rows, the CLI attaches those
intervals to the post-hull rungs, calls `apply_uncertainty_recipe()` and selects
knees from the adjusted set. With point-only rows it loads the thresholds and
uses `wide_interval_min_width` as the fallback interval width before running
the same recipe.

!!! note "What the extension does not change"
    The recipe affects which rungs the builder evaluates. It does not change
    the Netflix golden-data assertions, the `convex_hull` and `select_knees`
    invariants, or the HLS, DASH and JSON manifest schema.

## History

Fixes that shaped the current source handling, all dated 2026-05-18 unless
noted:

- **ADR-0505.** The encode driver used to pass `-f rawvideo -pix_fmt yuv420p
  -s WxH -i src.mp4` for every source, reading a container's compressed bytes
  as planar YUV and producing a uniformly bogus encode of about 50 Mbps with
  VMAF between 4 and 9, whatever the CRF. The corpus now detects containers by
  suffix (anything outside `{".yuv", ""}`), sets
  `EncodeRequest.source_is_container=True`, and FFmpeg auto-detects the format.
  The same change widened `samples[]` from one row per target cell to every CRF
  the sampler scored, de-duplicated by `(width, height, crf)`, so that
  `vmaf-tune report --ladder-json` can draw the Pareto cloud
  ([ADR-0501](../adr/0501-vmaf-tune-bbb-e2e-v4-bug-cluster.md) added the array).
- **ADR-0501.** The reference leg of a cross-resolution rung used to be decoded
  at the source geometry while libvmaf read both legs at the rung target. A
  1920x1080 reference was parsed as 1280x720 and scored about 21 instead of
  about 93, collapsing the ladder to one rendition. The per-rung
  `-vf scale=W:H` decode and the `<W>x<H>` sidecar name fixed it.
- **ADR-0498.** The default sampler used the rung target as the source size,
  which corrupted every encode of a raw YUV whose real size differed
  (`-s 1280x720` on 1080p bytes). `--src-width` and `--src-height` carry the
  real size, and a scale filter is injected for each sub-source rung.
- **ADR-0506.** `--duration` used to be metadata only: a 10-second probe of a
  9-minute container re-encoded the full source at every CRF. It now also
  bounds the encode pipe and the reference decode through input-side `-t`.
- **ADR-0497.** Added `--framerate`, `--pix-fmt` and `--crf-sweep`; the old
  hard-coded 24 fps gave wrong bitrate math on other frame rates.
- **ADR-0511 and ADR-0667.** Added `--score-backend` and `--vmaf-bin`; ADR-0667
  added `hip` and the native-first `auto` order.
- **ADR-0598 and ADR-0577.** Added `--workdir` and `--max-concurrent-decodes`
  for consistency with `compare`; until 2026-10-04 the sampler ignored both
  (its scratch directory went to the system temporary directory).

## See also

- [`vmaf-tune.md`](vmaf-tune.md) — overview of every subcommand.
- [`vmaf-tune-bitrate-ladder.md`](vmaf-tune-bitrate-ladder.md) — short
  introduction to Phase E.
- [`vmaf-tune-ladder-default-sampler.md`](vmaf-tune-ladder-default-sampler.md)
  — the sampler that fills the ladder.
- [`vmaf-tune-recommend.md`](vmaf-tune-recommend.md) — per-clip CRF search that
  consumes the same intervals.
- [`vmaf-tune-report.md`](vmaf-tune-report.md) — renders `samples[]`.
- [Ladder API](../api/ladder.md) — `convex_hull`, `select_knees`,
  `emit_manifest`.
- [`docs/ai/conformal-vqa.md`](../ai/conformal-vqa.md) — the conformal
  prediction surface (ADR-0393).
