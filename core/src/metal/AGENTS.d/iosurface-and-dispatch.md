---
paths:
  - core/src/metal/iosurface_layout.h
  - core/src/metal/picture_import.mm
  - core/src/metal/dispatch_strategy.c
  - core/src/metal/kernel_template.h
  - core/src/feature/metal/**
invariant: IOSurface bi-planar layout planning, exact dispatch feature-key allowlist, and HIP/Metal template symmetry.
---
# IOSurface import, dispatch allowlist, and template symmetry

## Rebase-sensitive invariants

- **IOSurface import reads surface layout itself** (fork-local,
  [ADR-1679](../../../../docs/adr/1679-metal-iosurface-biplanar-import.md)).
  `vmaf_metal_picture_import()` in `picture_import.mm` plans every plane
  through `iosurface_layout.h`: surface's CoreVideo pixel format picks
  layout (NV12 `420v`/`420f`, P010 `x420`/`xf20` shift 6, planar
  `y420`/`f420`); bi-planar planes 1 / 2 = even / odd samples of second
  plane; unknown format -> `-ENOTSUP`, wrong geometry -> `-EINVAL`, nothing
  copied. Header stays plain C: `test_metal_iosurface_layout` runs it on
  every host. FFmpeg patch `0013` imports planes 0..2 of both frames and
  fails on import error; `build_pictures` still requires `0x7`.
  **On rebase**: never restore copy of surface plane `n` as picture plane
  `n`; new accepted layout = table row + host test case + 4:2:0 ring check.

- **Metal dispatch allowlist uses extractor names and exact provided
  feature keys.** `g_metal_features[]` in `dispatch_strategy.c` carries both
  every registered Metal extractor's `.name` and each routable
  `provided_features[]` string. Do not abbreviate or infer these keys:
  mismatch makes `vmaf_metal_dispatch_supports()` return false and silently
  routes feature away from Metal. `test_metal_kernel_coverage_audit`
  guards extractor-name coverage and `test_metal_smoke` pins representative
  provided keys; add explicit assertion when new provided key lands.
  `check-dispatch-registry.sh` checks separate global extractor registry,
  not this string allowlist. Commit `53c8ef155` established this coupling.

- **`kernel_template.h` mirrors `hip/kernel_template.h` modulo
  unified-memory buffer collapse** (fork-local, ADR-0361).
  `VmafMetalKernelLifecycle` struct mirrors `VmafHipKernelLifecycle`
  field-for-field (one command-queue/stream slot + two event slots).
  Buffer struct `VmafMetalKernelBuffer` deliberately has one fewer
  slot than `VmafHipKernelReadback`: no `host_pinned` because Apple
  Silicon's unified memory makes `[buffer contents]` host-visible
  directly. Helper signatures
  (`vmaf_metal_kernel_lifecycle_init/_close`,
  `vmaf_metal_kernel_buffer_alloc/_free`,
  `vmaf_metal_kernel_submit_pre_launch`,
  `vmaf_metal_kernel_collect_wait`) parallel HIP names with
  `_buffer_` substituted for `_readback_` to reflect zero-copy
  posture. **On rebase / refactor**: if fork PR touches
  `hip/kernel_template.h`'s lifecycle (e.g. adds third event),
  walk diff onto `metal/kernel_template.h` +
  `kernel_template.c` before merging. Buffer-vs-readback name
  asymmetry is intentional and stays.

- **`integer_motion_v2_metal.c` mirrors `integer_motion_v2_hip.c`
  call-graph-for-call-graph** (fork-local, ADR-0361). Same
  `MotionV2StateMetal` / `MotionV2StateHip` field shape, modulo
  ping-pong buffer slots: one (single `MTLBuffer` with
  `MTLResourceStorageModeShared`) rather than two. Unified memory
  means previous-frame ref Y plane lives in same address space
  kernel reads. `VMAF_FEATURE_EXTRACTOR_TEMPORAL` flag and
  `flush()` callback contract stay aligned with HIP / CUDA twins.
  **On rebase**: if future PR drifts HIP twin's lifecycle (e.g.
  adds third buffer), update Metal twin in same PR.

- **`vmaf_fex_integer_motion_v2_metal` is registered without
  `VMAF_FEATURE_EXTRACTOR_METAL` flag bit set** (fork-local,
  ADR-0361; unchanged through T8-1b). Flag bit reserved in enum;
  consumer does not set it yet: picture buffer-type check in
  `vmaf_feature_extractor_context_extract` would route
  Metal-flagged extractor through (not-yet-existing) Metal
  buffer-type branch. T8-1c adds
  `VMAF_PICTURE_BUFFER_TYPE_METAL_DEVICE` tag and *then* sets flag
  on extractor. **On rebase**: if refactor touches picture
  buffer-type dispatch, leave Metal extractor's flags at
  `VMAF_FEATURE_EXTRACTOR_TEMPORAL` only until T8-1c. Same rationale
  as HIP twin's `VMAF_FEATURE_EXTRACTOR_HIP`-deferral posture.
