- Restore dev-MCP GPU visibility and startup resilience lost during the
  repository-layout reconciliation: probes now match anchored SYCL/HIP device
  records, the Compose healthcheck waits for a responsive NVIDIA driver when
  one is exposed, and libvmaf retries an output-file open once after `EINTR`.
