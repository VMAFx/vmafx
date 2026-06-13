- **docs(vmaf-tune)**: refreshed two stale `--encoder` help strings in
  `tools/vmaf-tune/src/vmaftune/cli.py`. The Phase-A corpus subcommand
  still said `codec adapter (Phase A: libx264 only)` and the Phase-D
  per-shot subcommand still said `Phase D scaffold: libx264 only`,
  both wrong since the corpus + per-shot code paths route through
  `get_adapter(args.encoder)` against the full registry of 20+
  adapters (`av1_amf`, `av1_nvenc`, `av1_qsv`, `av1_videotoolbox`,
  `h264_amf`, `h264_nvenc`, `h264_qsv`, `h264_videotoolbox`,
  `hevc_amf`, `hevc_nvenc`, `hevc_qsv`, `hevc_videotoolbox`,
  `libaom`, `prores_videotoolbox`, `svtav1`, `vvenc`, `x264`,
  `x265`, ...). The `choices=list(known_codecs())` argparse wiring
  already accepts every registered adapter. Help text now matches.
