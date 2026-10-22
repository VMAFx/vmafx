- **The FFmpeg guide says how to score exactly N frames.** Cutting a run with
  the output options `-frames:v N` or `-t` lets any `libvmaf*` filter score one
  more pair than `ffmpeg` outputs (FFmpeg n9.0.2 applies them after the
  filtergraph; measured in 4 to 7 of 12 runs with `-frames:v`, every run with
  `-t`). Trim both inputs with `trim=end_frame=N` instead
  ([Score exactly N frames](docs/usage/ffmpeg.md#score-exactly-n-frames)).
