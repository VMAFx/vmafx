- **`ghcr.io/vmafx/vmafx-node:<tag>-sycl` scores on Intel GPUs (ADR-3371).**
  The `node-sycl` image now carries a oneAPI build of `libvmaf` from the shared
  `scripts/ci/build-oneapi-libvmaf.sh` (the script the production oneAPI image
  runs), the oneAPI runtime libraries and Intel's GPU user-mode stack (Level
  Zero loader and driver, IGC, gmmlib). Before, it set `VMAFX_BACKEND=sycl`
  over a CPU-only build. The amd64 image is published with its licence
  notices (`production-node-sycl-image`), a CycloneDX SBOM, a cosign
  signature and provenance.
