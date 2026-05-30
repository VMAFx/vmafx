- **`docs/state.md` drift sweep 2026-05-30.** Moved 5 stale Open bug rows
  to Recently closed after verifying each is structurally resolved on
  master: (1) **T-LEGACY-RUNNER-ANSNR-BROKEN** — `AnsnrFeatureExtractor`
  deleted in PR #283 + ADR-0749 legacy-runner sunset; (2)
  **T-LEGACY-RUNNER-STUB-MISSING-2026-05-29** — `VmafLegacyQualityRunner`
  imports removed from `python/test/quality_runner_test.py` via the
  ADR-0749 sunset; (3) **T-VK-1.4-BUMP**, (4) **T-VK-CIEDE-F32-F64**, and
  (5) **T-VK-VIF-1.4-RESIDUAL-ARC** — all three superseded by
  [ADR-0726](docs/adr/0726-drop-vulkan-backend.md) (Vulkan backend
  dropped 2026-05-28 / PR #47): the entire `core/src/vulkan/`,
  `core/src/feature/vulkan/`, and `libvmaf_vulkan.h` surface was removed,
  structurally closing all three Vulkan blockers. Native CUDA / HIP /
  SYCL backends cover every vendor formerly served by Vulkan.
