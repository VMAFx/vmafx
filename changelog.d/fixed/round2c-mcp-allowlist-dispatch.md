### Fixed

Round-2c bug-hunt follow-ups (fork-local, golden-safe; off the metric path).

- **R2-4 (security)** C MCP `compute_vmaf` path allowlist (`core/src/mcp/compute_vmaf.c`): `validate_path()` `realpath(3)`-canonicalises caller paths and rejects (`-EACCES`) anything not under the same allowlisted roots the Python/Go MCP servers use (`<repo>/testdata`, `<repo>/python/test/resource`, `<repo>/model`, `/workspace/python/test/resource`, `$VMAF_MCP_ALLOW`). Component-boundary prefix test. Load-bearing test added.
- **R2-9b** `gpu_dispatch_env.cpp` no longer lets `std::bad_alloc` cross its `extern "C"` boundary (UB) on OOM — wrapped + mapped to the documented NULL contract, atomic-`ready`-last invariant preserved.
