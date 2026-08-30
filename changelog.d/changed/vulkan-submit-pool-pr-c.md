### Vulkan submit-pool migration PR-C (ADR-0354)

Migrated the four remaining Vulkan feature extractors
(`cambi_vulkan`, `ssimulacra2_vulkan`, `float_ansnr_vulkan`,
`moment_vulkan`) from per-frame `vkCreateFence` /
`vkAllocateCommandBuffers` / `vkAllocateDescriptorSets` to the
pre-allocated `VmafVulkanKernelSubmitPool` template (ADR-0256).

- `float_ansnr_vulkan` and `moment_vulkan`: 1-slot pool + single
  pre-allocated descriptor set written once at `init()` (all SSBO
  bindings are init-time-stable, eliminating per-frame
  `vkUpdateDescriptorSets`).
- `cambi_vulkan`: 1-slot pool reused sequentially across all
  `cambi_vk_run_record` calls within a frame (strictly sequential
  dispatch; per-dispatch descriptor allocation retained because
  bindings vary per call).
- `ssimulacra2_vulkan`: 1-slot pool reused across the 6-scale
  `ss2v_run_scale` loop (per-scale descriptor allocation retained;
  ssimulacra2 remains CPU-bound by design per ADR-0201).

Completes the submit-pool migration across the full Vulkan extractor
fleet (PR-A #563, PR-B, PR-C). All 4 kernels pass `places=4`
ULP-diff gate vs CPU reference.
