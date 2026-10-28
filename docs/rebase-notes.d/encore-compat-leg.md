## SVT Encore leg of the upstream FFmpeg check (2026-10-28)

- `uc_encore_graph` in `scripts/ci/upstream-consumer-lib.sh` reproduces the
  graph of SVT Encore's `CommandBuilder.kt` at commit
  `8dd7c596c51a8c31bae805e31ff0969564f68229`; moving the pin means re-reading
  `vmafParams` and the graph assembly there.
- The FFmpeg build of `upstream-ffmpeg-compat.sh` enables the `split` filter
  for this leg; keep it in `--enable-filter`.
- No C, Python harness or golden-data impact.
