## Added

- **`vmaf_context_get_backend()`** (`core/include/libvmaf/libvmaf.h`,
  `core/src/libvmaf.c`): new public C API function that returns the active
  compute backend of a `VmafContext` as a `enum VmafBackend` value.
  Returns `VMAF_BACKEND_UNKNOWN` (0) for CPU-only contexts; returns
  `VMAF_BACKEND_CUDA`, `VMAF_BACKEND_SYCL`, `VMAF_BACKEND_METAL`, or
  `VMAF_BACKEND_HIP` after the corresponding `vmaf_<backend>_import_state()`
  call. Fully additive — no existing API is changed. ADR-0804.
- **`enum VmafBackend`** (`core/include/libvmaf/libvmaf.h`): new public enum
  with values `UNKNOWN=0`, `CUDA=1`, `SYCL=2`, `METAL=3`, `HIP=4`,
  `VULKAN=5` (reserved; Vulkan backend removed in ADR-0726).
- **Unit tests** (`core/test/test_context.c`): two new test cases —
  CPU-only context returns `VMAF_BACKEND_UNKNOWN`; null-pointer guards return
  `-EINVAL`.
