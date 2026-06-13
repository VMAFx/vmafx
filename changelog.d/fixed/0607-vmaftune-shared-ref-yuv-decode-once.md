- `vmaf-tune compare` now decodes the reference YUV exactly once for the
  entire run instead of once per worker. The ADR-0577 / PR #1354 "aggressive
  cleanup" policy deleted the 118 GB shared reference after each bisect
  worker's `finally` block, causing every subsequent worker to re-decode it
  through the `--max-concurrent-decodes 1` semaphore (~3 min per decode).
  With 56 concurrent workers (14 encoders × 4 targets) this produced up to
  392 re-decodes and a ~9.7 h wall-clock run on the v14 BBB 1080p sweep
  without convergence. `_run_compare` in cli.py now decodes the reference
  once, passes the raw-YUV path to all workers via the new `pre_decoded_ref`
  parameter on `compare_codecs` / `compare_codecs_sweep`, and deletes it in a
  `try/finally` block after the pool shuts down. Workers see a `.yuv` src and
  skip their own reference decode. Peak disk usage remains bounded to one
  reference YUV (ADR-0607).
