- vulkan: bump instance + VMA `apiVersion` from `VK_API_VERSION_1_3` to
  `VK_API_VERSION_1_4` across the four pinned sites in
  [`core/src/vulkan/common.c`](core/src/vulkan/common.c) (lines 54,
  264, 374) and [`core/src/vulkan/vma_impl.cpp`](core/src/vulkan/vma_impl.cpp)
  (`VMA_VULKAN_VERSION 1003000` → `1004000`). This is "Step B" of the
  multi-step VIF / ciede API-1.4 fix chain documented in
  [ADR-0264](docs/adr/0264-vulkan-1-4-bump-blocked-on-fp-contraction.md) /
  [ADR-0269](docs/adr/0269-vif-ciede-precise-step-a.md) /
  [ADR-0273](docs/adr/0273-ciede-vulkan-nvidia-f32-f64-precision-gap.md).
  Step A (precise tags on `vif.comp` + `ciede.comp`) shipped in PR #346;
  Phase 3 (PR #511) closed Arc + RADV via the cross-subgroup
  release-acquire fix. NVIDIA RTX 4090 + driver 595.71.05 still fails
  `integer_vif_scale2` 45/48 at API 1.4 pending Phase 3c (PR #512). This
  PR is held DRAFT until Phase 3c lands and the cross-backend gate is
  green on all three lanes.
