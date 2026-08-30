### lusoris fork

- **mcp**: runtime v2 lands the Unix-domain-socket transport
  (`vmaf_mcp_start_uds` is no longer `-ENOSYS`) and replaces the
  v1 `compute_vmaf` placeholder with a real libvmaf scoring
  binding. The tool now accepts `reference_path`,
  `distorted_path`, `width`, `height`, optional `model_version`
  (default `vmaf_v0.6.1`) and returns
  `{score, frames_scored, model_version, pool_method="mean"}`.
  YUV420p 8-bit only; 10/12-bit and SSE / loopback-HTTP
  transport remain deferred to v3. See
  [ADR-0332](../../docs/adr/0332-mcp-runtime-v2.md).
