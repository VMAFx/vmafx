- **Vulkan backend now runs on Intel Arc / AMD iGPU / older NVIDIA
  GPUs** (ADR-0512, supersedes ADR-0492). The VIF compute shader is
  now shipped as two SPIR-V variants — `vif_fp64.comp` (double
  precision, matches CPU bit-for-bit) and `vif_fp32.comp` (single
  precision, auto-fallback). The runtime probes
  `VkPhysicalDeviceFeatures::shaderFloat64` at backend init and picks
  the matching variant; no user opt-in is required. Empirical VMAF
  delta on Netflix golden 576x324: fp64 path -7e-5 vs CPU,
  fp32 path -8e-5 (Intel Arc A380) / -9e-5 (AMD `gfx1036`) vs CPU.
  Bit-exact-strict workflows can pin the fp64 path via
  `--vulkan-require-fp64` (CLI) / `VmafVulkanConfiguration::require_fp64`
  (public C API); default is off. Closes the regression where
  `vmaf --backend vulkan` aborted on devices without `shaderFloat64`.
