Restored Vulkan/lavapipe parity coverage for integer motion by routing automatic
`motion` checks through `integer_motion_vulkan`, restoring that extractor's
CPU-compatible debug output default, and correcting CUDA / SYCL / Vulkan
`motion_v2` mirror padding to match the CPU reference.
