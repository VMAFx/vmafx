- MCP `list_backends` now probes the local `vmaf` binary via
  `--help` (looking for `--no_<backend>` disable flags) rather than
  grepping the `--version` banner — fixes a false `cuda=false` in
  the `vmaf-dev-mcp` container where CUDA is live but not banner-
  advertised. Per-process cached so `vmaf_score` does not fork a
  subprocess per call. (ADR-0509, Bug A)
- MCP default allowlist now includes
  `/workspace/python/test/resource` alongside the host-relative
  `<repo>/python/test/resource` so the Netflix golden YUVs at their
  canonical container-mount path work without needing
  `VMAF_MCP_ALLOW`. `VMAF_MCP_ALLOW` override is preserved and
  additive. (ADR-0509, Bug B)
- `vmaf-tune ladder` gained `--score-backend {cpu,cuda,sycl,vulkan,auto}`
  (default `auto`, sibling to the same flag on `compare` and
  `tune-per-shot`). Unavailable backends error out via
  `score_backend.select_backend()` before any encodes start; the
  resolved value threads through the corpus sampler as `vmaf
  --backend $name`. `tune-per-shot` keeps its existing
  `auto → None → libvmaf-picks` predicate contract; the asymmetry is
  documented inline. (ADR-0509, Bug C)
