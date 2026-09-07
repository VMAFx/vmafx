- **Percentile pooling reaches the Go bindings, the CLI docs and the GPU FFmpeg
  filters.** The C API gained `VMAF_POOL_METHOD_MEDIAN` / `_PERC5` / `_PERC10` /
  `_PERC20` separately (see the `percentile-pooling-methods` entry); this wires
  the consumers. `pkg/libvmaf` exposes them as a typed `PoolMethod` with a
  `String()` mapping to the on-the-wire names, so Go callers select a pooling
  method without touching cgo enums. The `libvmaf_sycl`, `libvmaf_vulkan` and
  `libvmaf_metal` FFmpeg filters accept the four new `pool` values, which the
  CPU `libvmaf` filter already did. `docs/usage/cli.md` documents them, and
  `python/test/command_line_test.py` covers them end to end through the CLI.
