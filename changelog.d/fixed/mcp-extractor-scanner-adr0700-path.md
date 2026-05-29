## MCP `list_extractors` scanner path corrected (ADR-0700 follow-up)

`_list_extractors()` in `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py` was still
scanning `libvmaf/src/feature/` (the pre-ADR-0700 path), which does not exist
after the directory rename to `core/`.  The scanner returned an empty list,
causing `test_list_extractors_returns_list` to fail with zero extractors found.

Changed scan root to `core/src/feature/`; also updated the stale `float_ansnr`
CPU assertion in the smoke test (the CPU-only variant was removed by ADR-0720;
only `float_ansnr_vulkan` remains).
