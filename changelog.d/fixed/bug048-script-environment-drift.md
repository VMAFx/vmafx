- Remove the retired `/home/kilian/dev/vmaf` fallback from the benchmark
  harness and make it resolve the checkout from its own location when
  `VMAF_ROOT` is unset.
- Keep GPU calibration help and manifest fixtures on the live CUDA/SYCL
  backend set after Vulkan's removal.
