- Remove the retired `/home/kilian/dev/vmaf` fallback from the benchmark
  harness and make it resolve the checkout from its own location when
  `VMAF_ROOT` is unset.
- Keep the benchmark harness and its linked backend-engagement guidance on the
  live CPU/CUDA/SYCL set instead of advertising the retired Vulkan path, and
  make its fixture guidance match the checkerboard input it actually processes.
- Record each run's emitted metrics-key counts and flag suspicious CPU/GPU
  count collapse without freezing backend-specific counts as constants.
- Keep GPU calibration help and manifest fixtures on the live CUDA/SYCL
  backend set after Vulkan's removal.
- Reject malformed calibration scorer JSON before attempting to pair frame
  metrics, while keeping the touched collector and tests strict-mypy clean.
- Preserve the `vmaf` CLI's explicit `backend_used` receipt in both Python and
  Go MCP auto scoring, and return `unknown` when no valid receipt exists,
  instead of guessing a backend from a mutable metrics-key count. Reject
  non-object score JSON before annotating the MCP response.
