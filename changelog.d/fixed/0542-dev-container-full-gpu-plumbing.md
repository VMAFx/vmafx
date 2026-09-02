- **dev-mcp container: all four GPU backends now run on real
  hardware** (ADR-0541). The container's entrypoint
  (`dev/scripts/dev-mcp-entrypoint.sh`) rewrites `VK_DRIVER_FILES` at
  startup to the colon-separated list of non-lavapipe Vulkan ICD JSONs
  visible under `/etc/vulkan/icd.d/` + `/usr/share/vulkan/icd.d/`, so
  `vmaf --backend vulkan --vulkan_device 0` lands on the host's NVIDIA
  / Intel / AMD adapter instead of mesa's lavapipe software emulator.
  `dev/docker-compose.yml` pins `HSA_OVERRIDE_GFX_VERSION=10.3.0`,
  `HSA_ENABLE_SDMA=0`, and `ROCR_VISIBLE_DEVICES=0` in the shared
  `common-env` block so AMD `gfx1036` (Raphael iGPU, not on ROCm 6.x's
  default supported-GPU allowlist) passes `hsa_init()` and the HIP
  backend dispatches kernels instead of failing with
  `HSA_STATUS_ERROR_OUT_OF_RESOURCES`. Stage 1 of `dev/Containerfile`
  picks up `intel-media-va-driver-non-free` + `mesa-va-drivers` so the
  VA-API codec drivers Intel compute-runtime and AMD radeonsi dlopen
  during GPU capability probing are present. The
  `NVIDIA_DRIVER_CAPABILITIES=compute,graphics,utility,video` invariant
  (the `graphics` token triggers the Container Toolkit's
  `nvidia_icd.json` bind-mount) is documented inline in
  `dev/docker-compose.yml` and `dev/AGENTS.md`. Acceptance:
  `for B in cpu cuda sycl vulkan hip; do vmaf --backend $B …; done`
  inside the rebuilt container returns a real score per backend with
  no `Using CPU` silent-fallback line on the dev machine (NVIDIA RTX
  4090 + Intel Arc A380 + AMD `gfx1036`).
