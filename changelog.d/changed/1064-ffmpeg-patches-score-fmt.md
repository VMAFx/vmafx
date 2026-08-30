- **feat(ffmpeg-patches):** Add `score_fmt` AVOption to all four vmaf FFmpeg
  filters (`libvmaf`, `libvmaf_sycl`, `libvmaf_vulkan`, `libvmaf_metal`).
  Replaces hard-coded `vmaf_write_output()` calls with
  `vmaf_write_output_with_format()`, exposing the fork's `--precision` flag
  semantics at the FFmpeg integration layer. Users can now write
  `libvmaf_sycl=log_path=out.xml:score_fmt=%.17g` to get IEEE-754
  lossless log output, equivalent to `vmaf --precision=max` on the CLI.
  Default (`score_fmt=NULL`) preserves the existing `%.6f` behaviour.
  New series patch: `0016-libvmaf-wire-score-fmt-on-all-vmaf-filters.patch`.
  (ADR-1064)
