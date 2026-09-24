- Replace all `nvidia/cuda` container base images with digest-pinned Ubuntu 26.04
  and exact NVIDIA apt installations via hardened `scripts/ci/install-cuda-toolkit.sh`
  (`--mode=builder`, `--mode=runtime`, and shared dev-container `--mode=full`).
  Every core apt operand and installed dpkg version is locked to live NVIDIA
  metadata, unblocking CUDA bumps from upstream OCI image publication lag without
  floating within the 13.4 package series (#1525, ADR-1306).
- Move Renovate's CUDA release discovery from `nvidia/cuda` Docker tags to NVIDIA's
  official redist HTML index, with fail-closed version extraction, an exact-package
  metadata review latch, and manual review.
