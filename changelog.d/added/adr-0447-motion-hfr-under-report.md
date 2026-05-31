- **ADR-0447** — documents the decision to correct motion-feature under-reporting
  on HFR (50p+) content by applying `motion_fps_weight = clamp(30/fps, 0.25, 4.0)`
  across all motion extractor variants (CPU, CUDA, SYCL, Vulkan). Closes Issue #837
  partially; implementation ships in PR #851.
