- **Embedded MCP server — v1 stdio runtime
  ([ADR-0209](../docs/adr/0209-mcp-embedded-scaffold.md)
  § "Status update 2026-05-08").** Promotes the T5-2 `-ENOSYS`
  scaffold to a working JSON-RPC 2.0 server inside `libvmaf.so`.
  `vmaf_mcp_init` / `vmaf_mcp_start_stdio` / `vmaf_mcp_stop` /
  `vmaf_mcp_close` are wired; the dispatcher routes
  `initialize`, `tools/list`, `tools/call`, and `resources/list`
  with two tools shipped — `list_features` (real, walks the
  feature-extractor registry) and `compute_vmaf` (placeholder —
  validates `reference_path` / `distorted_path` and returns a
  deferred-to-v2 marker; binding to the scoring engine lands in
  v2). Vendors cJSON v1.7.18 (MIT) under
  `core/src/mcp/3rdparty/cJSON/`. SSE and UDS transports
  remain `-ENOSYS` and ship with the v2 mongoose vendoring PR;
  v1 stdio uses line-delimited JSON-RPC, with LSP `Content-Length:`
  framing also deferred to v2. Smoke test
  (`core/test/test_mcp_smoke.c`) flipped from pinning the
  scaffold contract to driving 15 end-to-end JSON-RPC
  round-trips over a `pipe(2)` pair. Build flag unchanged
  (`-Denable_mcp=true -Denable_mcp_stdio=true`); default off.
