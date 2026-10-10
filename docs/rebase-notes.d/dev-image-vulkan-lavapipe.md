## Dev image: Vulkan loader and lavapipe (2026-10-10)

- `dev/Containerfile`, stage `build-deps`, installs `libvulkan1`,
  `libvulkan-dev`, `mesa-vulkan-drivers` (lavapipe) and `vulkan-tools`, and a
  build step fails when `vulkaninfo --summary` lists no `llvmpipe`
  (ADR-3137, operator decision ci-config-19). Fork-only: upstream Netflix/vmaf
  has no dev image. **On sync**: keep the four packages, the package check and
  the lavapipe step; do not move them into a second image.
