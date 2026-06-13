## `docs/mcp`: tools catalogue completeness + duplicate-section cleanup + ADR-ref fixes

- `docs/mcp/index.md`: tool catalogue expanded from 7 to 15 entries — added
  `vmaf_score_encoded`, `probe_backend`, `vmaf_version`, `run_compare`,
  `run_ladder`, `run_tune_per_shot`, `list_extractors`, and `describe_model`.
- `docs/mcp/tools.md`: removed duplicate `## Cross-tool error conventions`
  section (stale legacy `TextContent` shape superseded by ADR-0613).
- `docs/mcp/tools.md`: removed duplicate `[ADR-0100]` link in `## Related`.
- `docs/mcp/tools.md`: corrected seven `ADR-0608` references to `ADR-0638`
  (ADR-0608 is the Zed editor config; ADR-0638 is the MCP P1 surface decision).
- `docs/mcp/http-transport.md`: `/v1/score` `bitdepth` field updated from
  `8 or 10` to `8 | 10 | 12 | 16` to match the canonical `vmaf_score` tool docs.
- `docs/mcp/release-channel.md`: "two MCP server flavours" corrected to
  "three"; Go binary (`vmafx-mcp`) entry and release checklist added.
