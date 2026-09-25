- Remove the retired `/home/kilian/dev/vmaf` fallback from the benchmark
  harness and make it resolve the checkout from its own location when
  `VMAF_ROOT` is unset.
- Keep the benchmark harness and its linked backend-engagement guidance on the
  live CPU/CUDA/SYCL set instead of advertising the retired Vulkan path, and
  make its fixture and metrics-key guidance match the checkerboard/SYCL output
  it actually processes.
- Keep GPU calibration help and manifest fixtures on the live CUDA/SYCL
  backend set after Vulkan's removal.
- Reject malformed calibration scorer JSON before attempting to pair frame
  metrics, while keeping the touched collector and tests strict-mypy clean.
