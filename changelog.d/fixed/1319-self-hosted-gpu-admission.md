- **Self-hosted GPU admission**: `Coverage GPU` now runs only after a hosted
  probe confirms an online runner with its complete label set; enabled but
  unavailable hardware fails the required aggregator instead of waiting
  forever. The obsolete duplicate SYCL parity job is retired without treating
  the Arc-only runner as CUDA/HIP-capable.
