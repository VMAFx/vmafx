- **macOS Vulkan-via-MoltenVK CI lane (advisory)
  ([ADR-0338](../docs/adr/0338-macos-vulkan-via-moltenvk-lane.md)).**
  Adds `Build — macOS Vulkan via MoltenVK (advisory)` to
  [`libvmaf-build-matrix.yml`](../.github/workflows/libvmaf-build-matrix.yml).
  Validates the existing Vulkan compute backend
  ([ADR-0127](../docs/adr/0127-vulkan-compute-backend.md)) on Apple
  Silicon (`macos-latest`) via the MoltenVK Vulkan-on-Metal
  translation layer. Installs `molten-vk`, `vulkan-loader`,
  `vulkan-headers`, and `shaderc` via Homebrew; pins the loader to
  MoltenVK with
  `VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json`;
  builds with `-Denable_vulkan=enabled`; runs `test_vulkan_smoke`,
  `test_vulkan_pic_preallocation`, and
  `test_vulkan_async_pending_fence`. Lane is `continue-on-error:
  true` (advisory) until one green run on `master`. Operator-facing
  install + known-limitations documentation lives at
  [`docs/backends/vulkan/moltenvk.md`](../docs/backends/vulkan/moltenvk.md);
  feasibility-against-fork-shaders digest at
  [`docs/research/0089-moltenvk-feasibility-on-fork-shaders.md`](../docs/research/0089-moltenvk-feasibility-on-fork-shaders.md).
  Complementary to the native Metal backend (separate workstream)
  — MoltenVK is the cross-platform parity story, native Metal is
  the macOS performance story.
