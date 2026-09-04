- **MCP tiny-AI scoring surface and strict input validation**:
  Expose the full tiny-AI scoring flag suite (`tiny_model`, `tiny_device`,
  `dnn_ep` alias, `tiny_threads`, `tiny_fp16`, `tiny_model_verify`,
  `tiny_codec`, `tiny_preset`, `tiny_crf`, `tiny_resize`, `no_reference`)
  along with feature/CTC selection and thread controls on `vmaf_score`
  and `vmaf_score_encoded` across both Go (`cmd/vmafx-mcp`) and Python
  (`mcp-server/vmaf-mcp`) MCP servers. Input validation rejects unknown
  enums and out-of-range numeric values with byte-compatible argv parity.
