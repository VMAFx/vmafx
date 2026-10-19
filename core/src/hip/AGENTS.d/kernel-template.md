---
paths:
  - core/src/hip/kernel_template.h
  - core/src/hip/kernel_template.c
  - core/src/hip/hip_handle.h
invariant: kernel_template.{h,c} mirrors CUDA kernel template field-for-field and helper-for-helper.
---
# HIP Kernel Template Mirror and Lifecycle

- **`kernel_template.h` mirrors `cuda/kernel_template.h`** (fork-local,
  ADR-0241). Struct shapes (`VmafHipKernelLifecycle` ↔
  `VmafCudaKernelLifecycle`, `VmafHipKernelReadback` ↔
  `VmafCudaKernelReadback`) and helper signatures
  (`vmaf_hip_kernel_lifecycle_init/_close`,
  `vmaf_hip_kernel_readback_alloc/_free`,
  `vmaf_hip_kernel_submit_pre_launch`,
  `vmaf_hip_kernel_collect_wait`) are deliberately one-to-one with
  CUDA template. Any change to CUDA template (helper signatures,
  struct fields, semantics) needs paired HIP change in same PR —
  otherwise mirror drifts, consumer call sites diverge between two
  backends. **On rebase / refactor**: if upstream port or fork PR
  touches `cuda/kernel_template.h`, walk diff onto
  `hip/kernel_template.h` + `kernel_template.c` before merging. HIP
  variant is out-of-line (`.c` paired with `.h`) instead of
  `static inline` for reason documented in `kernel_template.h`'s
  preamble — keep split until runtime PR ships, then re-evaluate. See
  [ADR-0241](../../../../docs/adr/0241-hip-first-consumer-psnr.md).

- **Merge-conflict risk with PR #612**:
  `vmaf_hip_kernel_submit_post_record` in `kernel_template.{h,c}`
  and `hip_hsaco_sources` meson pipeline are also being added by PR
  #612 (`float_psnr_hip`). When two PRs merge, keep one copy,
  discard duplicate. Bodies identical so either direction safe.

- **`uintptr_t` handles convert only through `hip_handle.h`.**
  `kernel_template.h` and `libvmaf_hip.h` carry `hipStream_t` /
  `hipEvent_t` as `uintptr_t` (no `<hip/hip_runtime_api.h>` there,
  ADR-0241). `kernel_template.c`, `common.c` and `picture_hip.c` get
  typed handle from `vmaf_hip_stream_of()` / `vmaf_hip_event_of()` and
  bits from `vmaf_hip_stream_bits()` (union `VmafHipHandle`), never by
  integer-to-pointer cast. Include it only from TU that builds against
  HIP runtime.
