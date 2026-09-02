- **Metal (Apple Silicon) compute backend runtime (T8-1b)
  ([ADR-0420](../docs/adr/0420-metal-backend-runtime-t8-1b.md))**.
  Replaces the pure-C scaffold TUs from T8-1
  ([ADR-0361](../docs/adr/0361-metal-compute-backend.md)) with
  three Objective-C++ (`.mm`) implementations that drive
  `Metal.framework` directly:
  [`core/src/metal/common.mm`](../core/src/metal/common.mm)
  (MTLDevice + MTLCommandQueue lifecycle,
  `MTLCreateSystemDefaultDevice` for auto-pick,
  `MTLCopyAllDevices` for explicit indexing on macOS, Apple-Family-7
  gate),
  [`core/src/metal/picture_metal.mm`](../core/src/metal/picture_metal.mm)
  (MTLBuffer allocator using `MTLResourceStorageModeShared` —
  zero-copy unified memory on Apple Silicon), and
  [`core/src/metal/kernel_template.mm`](../core/src/metal/kernel_template.mm)
  (private MTLCommandQueue + two MTLSharedEvent handles per
  consumer, submit-side `[MTLBlitCommandEncoder fillBuffer:…value:0]`
  + cross-queue `encodeWaitForEvent`, collect-side drain via
  `[MTLCommandBuffer waitUntilCompleted]`). All `.mm` TUs compile
  with `-fobjc-arc`; C-struct slots that hold Metal handles are
  `void *` / `uintptr_t` populated via `__bridge_retained` and
  drained via `__bridge_transfer`, so `<Metal/Metal.h>` stays out
  of every header in `core/src/metal/` and out of every
  consumer TU under `core/src/feature/metal/` (header-purity
  contract from [ADR-0361 §"Header purity"](../docs/adr/0361-metal-compute-backend.md)).
  Two new internal accessors —
  `vmaf_metal_context_device_handle()` and
  `vmaf_metal_context_queue_handle()` — expose the bridge-retained
  `void *` slots to picture / kernel-template TUs without
  struct-layout coupling (same pattern as
  `vmaf_hip_context_stream()` /
  `vmaf_cuda_context_stream()`). The pure-C
  `dispatch_strategy.c` and the per-feature scaffolds under
  `feature/metal/` are untouched — consumer TUs remain pure-C.
  Smoke test
  [`core/test/test_metal_smoke.c`](../core/test/test_metal_smoke.c)
  flips from the T8-1 `-ENOSYS` pin to runtime expectations:
  `vmaf_metal_state_init`, `vmaf_metal_context_new`,
  `vmaf_metal_kernel_lifecycle_init`, and
  `vmaf_metal_kernel_buffer_alloc` each return `0` on
  Apple-Family-7+ devices and `-ENODEV` on every other host;
  input-validation paths (`NULL` arguments, non-zero `flags`)
  still fire unconditionally. The `motion_v2_metal` extractor
  stays at "registered but kernel not ready" — the first real
  kernel is T8-1c.
  Apple Silicon (GPU Family Apple 7+) only. Reproducer (on
  macOS): `meson setup build -Denable_metal=enabled && ninja -C
  build && meson test -C build test_metal_smoke`.
