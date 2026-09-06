- `tools/vmaf-tune/src/vmaftune/report.py`: resolve remaining 9 findings
  (#2–#10) of profile-report audit
  (`docs/research/vmaftune-profile-report-audit-2026-05-27.md`):
  - Axis units: explicitly label bitrate axes as `bitrate (kbps)` on
    compare bar chart and `bitrate (kbps, log scale; left is smaller)`
    on sweep chart; unify tick formatting to `Mbps` / `kbps`.
  - Failed rows: render CRF, bitrate, encode time, and VMAF as `—`
    instead of `0 kbps` or `0.00` when `not row.ok`.
  - Color palette: allocate slots 15–17 for `h264_videotoolbox`,
    `hevc_videotoolbox`, and `av1_videotoolbox` to avoid hue collisions
    with software encoders.
  - Pareto frontier: deduplicate labels to only the lowest-bitrate
    frontier point per codec, including bitrate context (`codec @ X Mbps`).
  - Legend: add `picked CRF` label to scatter plots and deduplicate legend
    entries.
  - Determinism: strip creation timestamps and set `svg.hashsalt` to
    guarantee byte-identical SVG and HTML output across repeated renders.
  - Failed targets: render dashed indicator and failure annotation in
    sweep chart.
- `tools/vmaf-tune/src/vmaftune/cli.py`: add `--json-sidecar` flag to
  `compare` and `report` subcommands to emit standalone `<output>.json`
  alongside HTML/Markdown reports.
