- **MCP `vmaf_score` / `vmaf_score_encoded` — tiny-AI, feature, and CTC
  parameter coverage
  ([ADR-1117](../docs/adr/1117-mcp-tiny-ai-feature-coverage.md)).** Both
  score tools (in the Go `cmd/vmafx-mcp` and Python `mcp-server/vmaf-mcp`
  servers) gain optional, backward-compatible parameters that map onto the
  corresponding `vmaf` CLI flags, closing the fork's largest MCP capability
  gap — the Tiny-AI / DNN scoring surface was previously **0 %** reachable
  over MCP. Added: the tiny-AI surface (`tiny_model`, `tiny_device` /
  `--dnn-ep`, `tiny_threads`, `tiny_fp16`, `tiny_model_verify`,
  `tiny_codec`, `tiny_preset`, `tiny_crf`, `tiny_resize`, and `no_reference`
  NR mode); feature selection (`feature`, a repeatable array, plus the
  `aom_ctc` / `nflx_ctc` Common-Test-Conditions presets); and score
  completeness (`threads`, `frame_cnt`, `frame_skip_ref` /
  `frame_skip_dist`, `no_prediction`). Every parameter is optional and
  forwarded to the CLI only when supplied, so existing callers are
  unaffected. No-reference mode makes the `ref` argument optional on
  `vmaf_score` and is gated on a tiny model being supplied (mirroring the
  CLI). The Go and Python tool schemas are verified byte-identical for both
  score tools. Documented in
  [docs/mcp/tools.md](../docs/mcp/tools.md#optional-scoring-parameters).
