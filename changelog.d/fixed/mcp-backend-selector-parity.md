- MCP server backend selector reaches parity with libvmaf CLI (ADR-0436).
  `vmaf_score` and `describe_worst_frames` now accept `vulkan`, `hip`,
  and `metal` in addition to the existing `cpu`/`cuda`/`sycl`/`auto`;
  `_list_backends()` reports all six as a host-probe of `vmaf --version`.
  Pre-PR, MCP clients passing `backend="vulkan"` (etc.) silently fell
  through to `auto`. Tests cover every backend selector.
