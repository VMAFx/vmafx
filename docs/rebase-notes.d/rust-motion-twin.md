## Rust `motion` twin mirrors `integer_motion.c` (2026-10-07)

`rc4/motion-twin`, #2096.

- `core/src/rust/feature/motion/src/{sad,window,extractor}.rs` port
  `motion_score_pipeline_8/16`, `motion_flush_one` / `vmaf_motion_window_flush`
  and `extract()` of `core/src/feature/integer_motion.c` (and `motion_blend()`)
  statement by statement. A sync that changes any of them changes the twin in the
  same PR; `scripts/ci/rust_twin_diff.py --feature motion` and the `sad`
  table test in `sad.rs` (values from the C pipelines) guard it. No score,
  public API or FFmpeg patch impact.
