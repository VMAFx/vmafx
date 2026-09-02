### MCP P1 surface: vmaf-tune integration, list_extractors, describe_model, progress notifications (ADR-0608)

Five new MCP tools added to `vmaf-mcp`:

- **`list_extractors`** — enumerate all `VmafFeatureExtractor` implementations from
  the C source tree (no binary required). Returns name, backend tag, and source file.
- **`describe_model`** — return metadata for a named VMAF model (type, feature list,
  size). Fixes a `Path.stem` bug where `vmaf_v0.6.1` was trimmed to `vmaf_v0.6`.
- **`run_compare`** — wrap `vmaf-tune compare`: compare codec adapters at target VMAF
  scores and return a ranked JSON report.
- **`run_ladder`** — wrap `vmaf-tune ladder`: build a per-title bitrate ladder and
  emit an HLS / DASH / JSON manifest.
- **`run_tune_per_shot`** — wrap `vmaf-tune tune-per-shot`: detect scene cuts and
  return per-shot CRF recommendations.

All four `run_*` tools (including the existing `run_benchmark`) now emit
`notifications/progress` when the client supplies `params._meta.progressToken`.
