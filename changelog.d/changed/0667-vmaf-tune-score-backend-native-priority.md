- `vmaf-tune --score-backend` now accepts explicit `hip` and chooses score
  backends in native-first `auto` order: CUDA, SYCL, HIP/ROCm, Vulkan, then CPU.
