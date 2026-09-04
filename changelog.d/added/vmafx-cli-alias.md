Ship the `vmafx` CLI alias and `--netflix-compat` flag per ADR-0690 and ADR-0696.
When invoked as `vmafx`, the CLI defaults to IEEE-754 round-trip lossless precision
(`%.17g`, `--precision=max`) and the modernized default model `vmaf_v1.0.16_3d0h`.
Passing `--netflix-compat` (or `--netflix_compat`) forces legacy Netflix CPU backend,
`%.6f` precision, and `vmaf_v0.6.1` default model. Python entry points `vmafx-train`,
`vmafx-tune`, and `vmafx-mcp` are also registered.
