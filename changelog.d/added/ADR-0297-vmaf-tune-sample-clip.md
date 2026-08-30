- **`vmaf-tune --sample-clip-seconds N` — opt-in sample-clip mode for
  the Phase A grid sweep (ADR-0297, builds on ADR-0237 Phase A).**
  Encodes and scores only the centre `N`-second window of each source
  per `(preset, crf)` cell instead of the full reference, scaling
  per-cell wall time roughly linearly with slice length (e.g. ~6x
  speedup at `N=10` against a 60-second source). FFmpeg input-side
  `-ss <start> -t <N>` cuts the rawvideo demuxer at the slice
  boundary; the libvmaf CLI's `--frame_skip_ref` / `--frame_cnt`
  mirror the same window on the score side so VMAF compares matching
  frames without slicing the reference YUV on disk. Centre-anchored
  placement (naive scaffold; smarter shot-aware placement via
  TransNet V2 is a follow-up). Each emitted JSONL row carries
  `clip_mode = "sample_<N>s"` or `"full"`, letting Phase B
  (target-VMAF bisect) and Phase C (per-title CRF predictor) filter,
  weight, or epilogue-rescore the chosen cell on the full source.
  Corpus schema bumps additively to `SCHEMA_VERSION = 2`.
  `bitrate_kbps` is computed against the encoded duration so
  sample-clip rows aren't biased low. Falls back silently to
  `clip_mode = "full"` when `N >= duration_s`. Expected accuracy
  delta: ~1–2 VMAF points on diverse content (mixed-shot trailers,
  sports, action), tighter (~0.3–0.5 points) on uniform content
  (single-shot interviews, animation). Default off; legacy callers
  see no behaviour change. User docs:
  [`docs/usage/vmaf-tune.md`](../docs/usage/vmaf-tune.md#sample-clip-mode).
