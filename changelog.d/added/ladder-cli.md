- **`vmaf-tune ladder` CLI subcommand
  ([ADR-0295](../docs/adr/0295-vmaf-tune-phase-e-bitrate-ladder.md)).**
  Surfaces the existing Phase E per-title bitrate ladder pipeline
  ([`ladder.py`](../tools/vmaf-tune/src/vmaftune/ladder.py)) as a
  runnable subcommand. Sweeps `(resolution × target-VMAF)`, builds the
  convex hull, picks K knees by `--spacing` (`log_bitrate` /
  `uniform`), emits an HLS / DASH / JSON master manifest. The ladder
  module shipped earlier (with [Research-0068](../docs/research/0068-vmaf-tune-phase-e-bitrate-ladder.md));
  this entry just exposes it on the CLI.
