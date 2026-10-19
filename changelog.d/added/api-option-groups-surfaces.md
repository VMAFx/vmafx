- Every scoring option is now defined once, in the option groups of
  `core/api/vmafx.toml`, and generated into each surface: the `vmaf`
  option table and `--help` text, the input schemas both MCP servers serve,
  the `ScoreOptions` proto message and its OpenAPI schema, the AVOption table
  of the coming `vmafx` FFmpeg filter (`ffmpeg-patches/src/vf_vmafx_options.h`)
  and the option tables of `docs/usage/cli.md`, `docs/usage/ffmpeg.md`,
  `docs/mcp/tools.md` and `docs/server/api-contract.md`. Every command-line
  spelling `vmaf` accepted before is still accepted (ADR-2044).
- The `vmaf` JSON report carries a `provenance` object (ABI version, active
  backend, extractor count, build version) next to `backend_used` (#2142).
- MCP scoring tools accept `view_distance` and `display_height` (the ADM
  extractor's viewing distance and display height) and declare the
  device-target arguments `target_width`, `target_height` and
  `target_scaling`, which accept only their defaults until device-targeted
  scoring lands.
