- Replace all `nvidia/cuda` container base images with digest-pinned Ubuntu 26.04
  and explicit NVIDIA apt installations via hardened `scripts/ci/install-cuda-toolkit.sh`
  (`--mode=builder` and `--mode=runtime`), unblocking CUDA bumps from upstream OCI
  image publication lag and bumping CUDA to 13.4.2 across the tree (#1525, ADR-1306).
- Move Renovate's CUDA release discovery from `nvidia/cuda` Docker tags to NVIDIA's
  official redist HTML index, with fail-closed version extraction and manual review.
