---
paths:
  - core/src/metal/**
invariant: Apple7+ device gate, Metal/Foundation probes, uintptr_t header boundary, and unified-memory template shape.
---
# Metal backend ground rules

## Ground rules

- **Parent rules** apply in full (see [../../AGENTS.md](../../../AGENTS.md)).
- **Apple Silicon only** — runtime PR (T8-1b) gates device
  selection on `MTLGPUFamily.Apple7` (M1 and later) via
  `-[id<MTLDevice> supportsFamily:]`. Intel Macs surface as
  `-ENODEV`. See ADR-0361 §"Apple Silicon-only" for rationale
  (Apple's discontinuation of Intel-Mac GPU parity, plus
  unified-memory zero-copy is load-bearing perf story).
- **Metal SDK is linked when `enable_metal=enabled`**. Since T8-1b
  (ADR-0420), `meson.build` declares `dependency('Foundation',
  required: true)` and `dependency('Metal', required: true)` (both
  gated behind `is_metal_enabled` condition). Non-macOS hosts are
  unaffected: Metal subdir only entered when
  `host_machine.system() == 'darwin'` and option is `enabled`
  or `auto` on macOS.
- **Metal runtime types cross headers as `uintptr_t`**. Public
  header `libvmaf_metal.h` and kernel template's
  `VmafMetalKernelLifecycle` / `VmafMetalKernelBuffer` carry
  `uintptr_t` for `id<MTLCommandQueue>` / `id<MTLBuffer>` /
  `id<MTLEvent>` so consumer TUs stay free of `<Metal/Metal.h>` /
  `<Metal/Metal.hpp>`. Cast on implementation side only.
  Mirrors HIP ADR-0212 and Vulkan ADR-0184 patterns.
- **Don't drift kernel-template field shape from HIP / CUDA
  beyond documented unified-memory simplification**. Metal
  template collapses (device, pinned-host) pair into single
  `MTLBuffer` with `MTLResourceStorageModeShared` because Apple
  Silicon has no separate device memory pool. Lifecycle struct
  (`cmd_queue` + `submit` + `finished` events) mirrors HIP /
  CUDA twins field-for-field. See "Rebase-sensitive invariants"
  below.
