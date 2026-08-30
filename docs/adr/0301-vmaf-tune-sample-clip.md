# ADR-0301: `vmaf-tune --sample-clip-seconds` (sample-clip mode)

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris
- **Tags**: tooling, ffmpeg, vmaf-tune, fork-local

## Context

[ADR-0237](0237-quality-aware-encode-automation.md) accepted the
Phase A scaffold for `tools/vmaf-tune/`: a grid sweep over
`(preset, crf)` cells that encodes the **full** source per cell, scores
the encode against the reference with the libvmaf CLI, and emits a
JSONL row per cell. With the multi-codec backlog (libx265, libsvtav1,
libvpx-vp9, libvvenc) and the per-title / per-shot search loops still
to come (Phases C–E), full-source encoding becomes a wall-time
bottleneck on long sources: a 60-second clip across a 6-cell grid is
six 60-second `libx264 slow` passes, which Phase C calibration sweeps
multiply by tens of (preset, crf) cells across many sources.

A representative slice typically reproduces the relative ordering of
encoder-parameter cells without the wall-time cost of a full encode.
The harness needs an opt-in mode that cuts the per-cell encode and
score to a fixed-length window of the source, **with the same window
on both sides** so VMAF stays valid (mismatched windows yield
nonsense scores). The fork-local consumers — Phase B (target-VMAF
bisect), Phase C (per-title CRF predictor) — need to know whether a
row was sampled or full so they can filter, weight, or epilogue-rescore
the chosen cell on the full source.

## Decision

We will add `--sample-clip-seconds N` to `vmaf-tune corpus` (default
`0` = legacy full-source). When set, the harness computes a
centre-anchored window `start = (duration_s − N) / 2`, prepends
`-ss <start> -t <N>` to FFmpeg's **input-side** options (so the
rawvideo demuxer fast-seeks the YUV by skipping `start * framerate`
frame-sized byte chunks instead of decoding past the slice), and
mirrors the same window on the score side via the libvmaf CLI's
`--frame_skip_ref` / `--frame_cnt`. Each emitted row carries
`clip_mode = "sample_<N>s"` or `"full"`. The corpus schema bumps to
`SCHEMA_VERSION = 2` (additive — `clip_mode` is the only new key).
`bitrate_kbps` is computed against the encoded duration (slice
seconds when sample-clip is on, source seconds otherwise) so
sample-clip rows aren't biased low by a slice-bytes ÷ source-seconds
ratio. If `N >= duration_s` the harness silently falls back to
`"full"` mode.

For the first scaffold, "representative" is **naive centre-anchored**
— shot-aware placement (e.g. via the existing TransNet V2 placeholder
to pick the slice with the highest spatial-temporal complexity) is on
the follow-up backlog and out of scope here.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Naive centre slice (chosen) | Simplest impl; one int math expression; deterministic; reproducible | Misses content variation if the centre window is uniform (single shot, static interview) | Picked: matches the task's "first scaffold, smarter sampling later" framing. Falls back to full automatically when source is too short. |
| Slice the reference YUV on disk into a temp file, then encode + score the temp | Simpler downstream — both sides see a "normal" source | Adds an I/O pass equal to slice-bytes per cell (defeats some of the speedup), pollutes scratch dir, requires temp-cleanup failure handling | Rejected. Mirroring the slice via `--frame_skip_ref` / `--frame_cnt` is zero-I/O on the reference and uses libvmaf CLI flags that already exist. |
| Output-side `-ss` (after `-i`) | One-line FFmpeg argv change | FFmpeg still decodes (rawvideo demuxer still reads) the full source up to `start`, then re-encodes only the slice — saves only encoder time, not demuxer / I/O time | Rejected. Defeats the linear-with-duration speedup that's the whole point. |
| Smart shot-aware placement (TransNet V2 + complexity score) | Picks the most-distinct window — better aggregate-VMAF predictor | Brings in the TransNet V2 dependency at the harness layer; needs a complexity scorer; out of scope for the scaffold | Deferred to a follow-up. The naive scaffold is the foundation; smart placement is one swap of `_resolve_sample_clip`. |
| Per-shot dynamic slicing (Phase D territory) | Closest to what a real per-shot CRF predictor wants | Phase D is its own ADR-0237 phase; conflating sample-clip and per-shot would block both | Out of scope. ADR-0237 keeps these separate phases. |

## Consequences

- **Positive**: Per-cell wall time scales with slice length, so
  `--sample-clip-seconds 10` on a 60-second source is roughly a 6x
  speedup per cell (encode + score). Phase C's multi-source
  calibration sweeps become tractable on sources longer than a few
  seconds. The `clip_mode` row tag lets Phase B/C consumers filter or
  weight sample rows without re-reading the source.
- **Negative**: Sample-clip VMAF drifts from full-clip VMAF by
  typically ~1–2 points on diverse content (mixed-shot trailers,
  sports, action), tighter (~0.3–0.5 points) on uniform content
  (single-shot interviews, animation). The relative ordering between
  cells survives — which is what the search loops actually consume —
  but the absolute number is noisier. Phase C should rescore the
  predictor's chosen cell on the full source as an epilogue.
- **Neutral / follow-ups**:
  1. Schema bumped to v2 (additive `clip_mode`); downstream consumers
     should read `clip_mode` defensively (default `"full"` for v1
     rows).
  2. Naive centre placement leaves shot-aware placement on the
     backlog — when TransNet V2 lands real weights, swap
     `_resolve_sample_clip` to pick the slice with the most distinct
     content.
  3. Bitrate semantics: rows now divide bytes by **encoded duration**,
     not source duration. Documented in `docs/usage/vmaf-tune.md`.

## References

- [ADR-0237](0237-quality-aware-encode-automation.md) — Phase A
  parent, defines the JSONL contract this ADR extends.
- [`docs/usage/vmaf-tune.md`](../usage/vmaf-tune.md) — user-discoverable
  flag + accuracy caveat documentation.
- libvmaf CLI: `--frame_skip_ref` / `--frame_cnt` — see
  `core/tools/cli_parse.c`.
- Source: `req` — direct user request 2026-05-03 to add a sample-clip
  mode to vmaf-tune that encodes only a short representative slice
  (e.g. 10 seconds) per grid point instead of the full video, with the
  naive middle-slice as the first scaffold and TransNet V2-based smart
  sampling as a follow-up.
