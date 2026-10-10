---
paths:
  - dev/Containerfile
  - .github/actions/tidy-lane/action.yml
invariant: Vulkan loader, headers, lavapipe in build-deps of the one dev image; build fails if vulkaninfo lacks llvmpipe.
---
<!-- markdownlint-disable MD013 -->
# Vulkan loader and lavapipe in the dev image (ADR-3137)

- `build-deps` installs `libvulkan1`, `libvulkan-dev`, `mesa-vulkan-drivers` (lavapipe, CPU Vulkan driver) and `vulkan-tools` from Ubuntu archive of digest-pinned `DEV_BASE`; same unpinned-package rule as rest of stage. Operator decision ci-config-19.
- Purpose: meson finds `dependency('vulkan')` in every lane built in image, so Vulkan frame-import tests build and hosted tidy lanes (`.github/actions/tidy-lane`, pinned image digest) measure them; Vulkan device without GPU.
- Build check after package check: `vulkaninfo --summary` must list `deviceName = llvmpipe`; build fails otherwise. Keep it; build host has no `/dev/dri`, so check is CPU-only case of hosted lanes.
- No second image, no tidy exception for Vulkan tests. Real-GPU Vulkan runs stay separate signal.
- Image change lands first; publish workflow pushes new digest; pin bump of tidy-lane action goes with PR that re-measures baselines.
