- **`vmaf` reads its two inputs ahead of scoring, on one thread each
  (ADR-1366).** The CLI used to read the reference frame, then the distorted
  frame, then score the pair, all on one thread; at 3840x2160 the two reads
  cost about 7 ms per frame whatever the backend did. Each input now has a
  reader thread that stays up to two frames ahead, so reading overlaps scoring
  and the two files are read at the same time. At 3840x2160 8-bit 4:2:0,
  `--feature psnr` drops from about 7-8 to about 3.5-4 ms per frame on the CPU,
  serial or with `--threads 16`, and `psnr`, `motion` and `adm` on an Arc B580
  from about 8 to about 4; runs limited by extraction keep their speed.
  Scores, frame order, `--frame_cnt`, `--frame_skip_*`, the progress line and
  the exit codes are unchanged, and the JSON is identical at
  `--precision max`. The picture pool holds four more pictures (about 50 MB at
  4K 8-bit). Inputs that may share a read position are still read on the main
  thread: on Linux and macOS the same file or pipe on both sides and
  `--no-reference`, on Windows anything but two regular files. See
  [Input read-ahead](docs/usage/cli.md#input-read-ahead).
