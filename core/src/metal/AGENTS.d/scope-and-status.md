---
paths:
  - core/src/metal/common.h
  - core/src/metal/common.mm
  - core/src/metal/picture_metal.h
  - core/src/metal/picture_metal.mm
  - core/src/metal/dispatch_strategy.c
  - core/src/metal/kernel_template.mm
  - core/src/metal/stubs.c
  - core/src/metal/meson.build
invariant: Metal runtime scope, Apple7+ context/picture/template status, and landed kernel dispatch coverage.
---
# Metal runtime scope and backend status

## Scope

Metal-side runtime (picture lifecycle, dispatch strategy, kernel
scaffolding template). Metal **feature kernels** live one level
deeper in [../feature/metal/](../../feature/metal/).

```text
metal/
  common.{mm,h}          # Metal context + command-queue management (T8-1b / ADR-0420)
  picture_metal.{mm,h}   # VmafPicture on Metal device — MTLBuffer lifecycle (T8-1b)
  dispatch_strategy.{c,h} # Feature-name → landed-kernel support predicate
  kernel_template.{mm,h} # per-feature kernel scaffolding + runtime (T8-1b / ADR-0420)
  stubs.c                # -ENOSYS fallbacks for public libvmaf_metal.h
                         #   entry points when HAVE_METAL is OFF. Wired in
                         #   from core/src/meson.build's `else` branch of
                         #   `if is_metal_enabled`, NOT via `subdir('metal')`.
                         #   Mirrors core/src/dnn/dnn_api.c's VMAF_HAVE_DNN
                         #   stub pattern.
  meson.build           # subdir() include from core/src/meson.build
```

## Backend status

**Runtime landed** — T8-1b
([ADR-0420](../../../../docs/adr/0420-metal-backend-runtime-t8-1b.md)):

- `common.mm` — `vmaf_metal_context_new` / `_destroy` /
  `vmaf_metal_available` / `vmaf_metal_list_devices` /
  `vmaf_metal_state_init` / `_import` / `_free`. Uses
  `MTLCreateSystemDefaultDevice()` for `device_index = -1`,
  `MTLCopyAllDevices()` for explicit indexing; gates on
  `[device supportsFamily:MTLGPUFamilyApple7]`. All `.mm` TUs
  compile with `-fobjc-arc`; Metal handles cross Obj-C++ / pure-C
  boundary as `void *` / `uintptr_t`.
- `picture_metal.mm` — `vmaf_metal_picture_alloc` /
  `vmaf_metal_picture_free`. Allocates `MTLBuffer` with
  `MTLResourceStorageModeShared` (zero-copy unified memory on
  Apple Silicon).
- `kernel_template.mm` — full lifecycle: private
  `MTLCommandQueue`, two `MTLSharedEvent` handles (submit-fence +
  finished-fence), per-frame `[MTLBlitCommandEncoder
  fillBuffer:range:value:0]` accumulator zero, cross-queue
  `encodeWaitForEvent`, collect-side drain via
  `[MTLCommandBuffer waitUntilCompleted]`.
- Two internal accessors added to `common.h`:
  `vmaf_metal_context_device_handle()` and
  `vmaf_metal_context_queue_handle()` expose bridge-retained
  `void *` slots to consumer TUs (same pattern as
  `vmaf_hip_context_stream()`).
- Smoke test `test_metal_smoke.c` flipped from T8-1 `-ENOSYS`
  pin to runtime expectations: on Apple-Family-7+ every entry
  point returns `0`; on every other host returns `-ENODEV`;
  input-validation paths still fire unconditionally.

**Batch-1 kernels landed** — T8-1c through T8-1j
([ADR-0421](../../../../docs/adr/0421-metal-first-kernel-motion-v2.md)):

- `motion_v2_metal`, `float_psnr_metal`, `float_moment_metal`,
  `integer_psnr_metal`, `float_motion_metal`,
  `integer_motion_metal`, and `float_ssim_metal` are Obj-C++ host
  dispatch files backed by `.metal` shaders and embedded
  `default.metallib`.
- `dispatch_strategy.c` no longer inert. Answers support for both
  extractor names and provided feature keys for those landed
  kernels; returns 0 for NULL contexts, NULL names, or unknown
  features.
- Remaining kernel ports (VIF, ADM, CIEDE, CAMBI, SSIMULACRA2, ...)
  follow as their own PRs gated by `places=4` cross-backend-diff
  lane (per [ADR-0214](../../../../docs/adr/0214-gpu-parity-ci-gate.md)).
