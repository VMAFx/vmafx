- **MCP scoring surface completeness (#1240)** exposes device selectors
  (`cpumask`, `gpumask`, `sycl_device`, `hip_device`, `metal_device`) and
  serialization format options (`output_fmt`: `json`, `xml`, `csv`, `sub`)
  in both Go (`cmd/vmafx-mcp`) and Python (`mcp-server/vmaf-mcp`) servers,
  resolves schema asymmetry by adding `subsample` to `vmaf_score`, adds strict
  enum and bounds input validation for all pass-through scoring extras, and
  introduces cross-server argv parity tests. See
  [docs/mcp/tools.md](../docs/mcp/tools.md).
