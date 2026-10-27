## SVT Encore cases in the upstream FFmpeg compat check (2026-10-28)

- `scripts/ci/upstream-ffmpeg-compat.sh`: `ff_encore_graph()`,
  `ff_encore_case()`, `ff_encore_unknown_option()` and `ff_encore_leg()`
  (run with `--against-cli`), and `split` in the minimal FFmpeg filter list.
  Fork-only CI tooling, nothing upstream to merge.
