- docs: API reference and backends pages now include Vulkan/HIP/Metal headers
  and the verified Metal scaffold count (8/17 registered, not 4/17). Updates
  `docs/api/index.md`, `docs/api/gpu.md`, `docs/backends/index.md`, and
  `docs/backends/metal/index.md` to match what
  `core/include/libvmaf/libvmaf_{vulkan,hip,metal}.h` and
  `core/src/feature/metal/*.mm` actually ship.
