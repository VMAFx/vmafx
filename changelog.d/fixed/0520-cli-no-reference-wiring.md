- `vmaf --no-reference` now actually skips the reference (ADR-0520).
  Prior to this change the flag was parsed into
  `CLISettings::no_reference` but never read downstream, so every NR
  invocation tripped the unconditional `Reference .y4m or .yuv
  (-r/--reference) is required` gate — a documented no-op surfaced by
  the e2e test matrix v9 (`.workingdir/bbb_reports/E2E_TEST_MATRIX_v9.md`
  Finding 8). The CLI now gates the reference-required check on
  `!no_reference`, requires `--tiny-model` in NR mode (no classic NR
  scorer exists in the fork), force-enables `--no_prediction` to
  suppress the built-in `vmaf_v0.6.1` SVM (it consumes FR feature
  columns), and opens the distorted source twice so the rank-4 tiny
  model dispatch in `vmaf_ctx_dnn_run_frame_nchw` sees the distorted
  frame in the "ref" slot. No public-API change; ffmpeg-patch stack
  untouched. Includes a C unit test for the success path and a shell
  smoke for the rejection diagnostic + end-to-end CLI gate.
