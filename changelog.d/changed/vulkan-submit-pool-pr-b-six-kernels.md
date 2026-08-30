- **Vulkan submit-pool migration PR-B: six secondary kernels** — migrates
  `ssim_vulkan.c`, `ciede_vulkan.c`, `ms_ssim_vulkan.c`, `motion_v2_vulkan.c`,
  `float_psnr_vulkan.c`, and `float_motion_vulkan.c` from per-frame
  `vkAllocateCommandBuffers` / `vkCreateFence` / `vkAllocateDescriptorSets` to
  the pre-allocated submit pool pattern introduced by PR #563 (PR-A, ADR-0256).
  Stable-binding kernels (`ssim`, `ciede`, `float_psnr`) call `vkUpdateDescriptorSets`
  once at `init()`, eliminating it from the hot path (T-GPU-OPT-VK-4). Ping-pong
  kernels (`motion_v2`, `float_motion`) retain one descriptor update per frame.
  `ms_ssim` receives two pools (`sub_pool_decimate` + `sub_pool_ssim`) and
  pre-allocates all 13 descriptor sets at init. No SPIR-V changes; numerical
  output is bit-exact with the previous implementation. Four remaining kernels
  (`ansnr`, `vif`, `ssimulacra2`, `cambi`) are deferred to PR-C. [ADR-0353]
