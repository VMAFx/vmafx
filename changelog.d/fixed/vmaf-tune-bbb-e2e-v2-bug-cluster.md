`vmaf-tune` + `libvmaf` CLI: closes the BBB end-to-end v2 bug cluster
(ADR-0498). `score._decode_to_raw_yuv` now honours
`ScoreRequest.duration_s` so a 10 s probe against a 634 s 1080p source
materialises ~896 MB of raw YUV instead of ~58 GB (Bug #v2-A). The
ladder default sampler takes separate `src_width / src_height` and
injects `-vf scale=W:H` for cross-resolution rungs against raw YUV
sources, fixing the "no scorable encodes" crash (Bug #v2-B). The
`dev-mcp` container image now bakes in `matplotlib` so `vmaf-tune
report` works inside the container per ADR-0496 (Bug #v2-C); the
report's chart helpers also fall back to a placeholder when
matplotlib is missing. The report's `<details>` JSON appendix uses
`allow_nan=False` with NaN→`null` coercion so strict RFC 8259
parsers accept it (Bug #v2-D). The `vmaf` CLI now refuses to silently
fall back to CPU when an explicit `--backend NAME` is requested and
that backend fails to initialise, and amends the JSON output with a
top-level `"backend_used"` echo so CI gates and MCP probes can
confirm dispatch (Bug #v2-E). Operational follow-ups: bisect
distinguishes "encoder unavailable" from genuine encode failures,
and a `ffmpeg -version` configure-line fallback populates
`encoder_version` for libx264 / libsvtav1 when their per-encoder
banner is suppressed by `-hide_banner`.
