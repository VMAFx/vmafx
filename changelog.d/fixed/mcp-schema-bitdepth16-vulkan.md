- **MCP schema correctness.** The `vmaf_score` / `describe_worst_frames` MCP
  tools rejected 16-bit input — their `bitdepth` enum was `[8,10,12]` while the
  CLI accepts `8/10/12/16` (verified `core/tools/vmaf.c` + `cli_parse.c`); added
  `16` in both the Python and Go MCP servers. Also removed the stale `vulkan`
  backend from `docs/mcp/tools.md` enums/examples (Vulkan was dropped in
  ADR-0726). MCP `backend` enum is `auto/cpu/cuda/sycl/hip/metal`.
