---
paths:
  - core/src/feature/feature_extractor.h
invariant: Per-frame malloc or aligned_malloc for geometry-sized buffers is strictly prohibited.
area: framework
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Geometry-Sized Buffer Heap Allocation Prohibition

- **Per-frame `malloc`/`aligned_malloc` for geometry-sized buffers is forbidden
  on hot paths** (ADR-0452): any buffer whose size is determined by input
  geometry (`w`, `h`, `stride`) MUST be hoisted to `init_fex` and freed in
  `close_fex`. geometry is known at init time. Per-frame heap traffic for
  geometry-sized scratch eliminates up to ~79 MB/frame of allocator pressure at
  1080p and causes arena lock contention in threaded mode. Examples: `float_vif`
  hoists `10 × plane_sz` to `VifState::vif_buf` per ADR-0452; `ssimulacra2`
  hoists its workspace similarly. If upstream port re-introduces per-frame
  allocation for geometry-sized buffer, move it to init/close in same PR.
  Small constant-size (geometry-independent) allocations inside hot paths
  are acceptable but must be justified in PR description.
