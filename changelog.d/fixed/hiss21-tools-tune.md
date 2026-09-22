- `vmaf-tune` and `vmaf-roi-score` Python sources and tests brought
  under the HISS-04 / NASA Rule 4 60-LOC function-length limit
  (43 violations cleared). Oversized functions were split into
  cohesive module-private helpers — argparse registration groups in
  `vmaf-roi-score`'s CLI, per-collection deserialisers for
  `ReportData.from_dict`, per-phase chart builders for the
  rate-quality and per-shot plots, per-section renderers for the
  Markdown and HTML report bodies, and shared runner/fixture
  factories across the test suites. No behaviour change: rendered
  reports, charts, encoder profiles, `build_ffmpeg_command` argv and
  every operator-facing log line are byte-identical to before.
