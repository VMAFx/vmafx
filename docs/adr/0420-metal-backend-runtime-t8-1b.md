<!-- markdownlint-disable MD013 MD037 MD060 -->
# ADR-0420: Metal backend runtime (T8-1b)

- **Status**: Accepted
- **Date**: 2026-05-11
- **Deciders**: lusoris, lawrence, Claude (Anthropic)
- **Tags**: `gpu`, `metal`, `apple-silicon`, `runtime`, `fork-local`

## Context

[ADR-0361](0361-metal-compute-backend.md) (T8-1) landed the Metal scaffold: public header [`core/include/libvmaf/libvmaf_metal.h`](../../core/include/libvmaf/libvmaf_metal.h), the backend tree under [`core/src/metal/`](../../core/src/metal/) with `common.c`, `picture_metal.c`, `dispatch_strategy.c`, `kernel_template.c`, and eight feature-kernel scaffolds in [`core/src/feature/metal/`](../../core/src/feature/metal/). Every entry point returned `-ENOSYS`. The scaffold's purpose was to fix the C-level surface ahead of any runtime work so consumers and CI lanes could land without churn.

A contributor reported a contradictory state from a user perspective: a Mac build of the fork has no working GPU acceleration today. The Vulkan-via-MoltenVK path covers most of the gap (the [Lusoris Homebrew tap's `libvmaf` formula](https://github.com/lusoris/homebrew-tap/blob/master/Formula/libvmaf.rb) ships it as the default on macOS) but it routes every SPIR-V kernel through MoltenVK's Vulkan → Metal translation layer, paying translation overhead and a couple of extension gaps (`atomicInt64`, external memory). The endgame for macOS is the native Metal backend per ADR-0361 §"Apple Silicon-only"; this PR closes the runtime half of that gap.

## Decision

We will replace the pure-C scaffold TUs in `core/src/metal/` with Objective-C++ (`.mm`) implementations that drive `Metal.framework` directly. The public-header ABI (handles cross as `uintptr_t` / `void *`) stays verbatim — the scaffold's purpose was to pin it, and the runtime PR respects it.

### Three `.mm` TUs

- [`core/src/metal/common.mm`](../../core/src/metal/common.mm) — `MTLDevice` + `MTLCommandQueue` lifecycle. `MTLCreateSystemDefaultDevice()` for `device_index = -1`; `MTLCopyAllDevices()` for explicit indexing on macOS (no-op on iOS). Apple-Family-7 gate via `[device supportsFamily:MTLGPUFamilyApple7]` — Intel Macs, non-Apple hosts, and pre-M1 iOS surface as `-ENODEV` from both `vmaf_metal_context_new` and `vmaf_metal_state_init`.
- [`core/src/metal/picture_metal.mm`](../../core/src/metal/picture_metal.mm) — `MTLBuffer` allocator with `MTLResourceStorageModeShared` (zero-copy unified memory on Apple Silicon).
- [`core/src/metal/kernel_template.mm`](../../core/src/metal/kernel_template.mm) — private `MTLCommandQueue` + two `MTLSharedEvent` handles per consumer; per-frame submit-side `MTLBlitCommandEncoder fillBuffer` + cross-queue `encodeWaitForEvent`; collect-side drain via `commandBuffer waitUntilCompleted`. Mirrors `hip/kernel_template.c`'s sequence one-to-one modulo the unified-memory buffer collapse.

### Memory ownership: ARC + bridge casts

All three `.mm` TUs compile with `-fobjc-arc`. C-struct slots that hold Metal handles are `void *` (or `uintptr_t` for the kernel-template ABI) populated via `(__bridge_retained void *)id` (id → void *, +1 retain) and drained via `(__bridge_transfer id)void *` (void * → id, -1 retain) on destroy/free. This keeps `<Metal/Metal.h>` out of every header in `core/src/metal/` and out of every consumer TU under `core/src/feature/metal/`, honouring the [ADR-0361 §"Header purity"](0361-metal-compute-backend.md) contract.

### Internal accessor pair, not struct-layout coupling

`picture_metal.mm` and `kernel_template.mm` need the device + queue handles that `common.mm` stashes on the context. We expose them via two accessors added to [`core/src/metal/common.h`](../../core/src/metal/common.h):

```c
void *vmaf_metal_context_device_handle(VmafMetalContext *ctx);
void *vmaf_metal_context_queue_handle(VmafMetalContext *ctx);
```

Both return the bridge-retained `void *` — caller never releases. Same pattern as `vmaf_hip_context_stream()` (ADR-0212) and `vmaf_cuda_context_stream()` (ADR-0246). Earlier drafts mirrored the `common.mm` struct layout from a "local layout" replica in `picture_metal.mm`; that was struct-layout coupling and was rejected (see Alternatives considered).

### Build wiring

[`core/src/metal/meson.build`](../../core/src/metal/meson.build) gains:

- `.mm` source entries for the three runtime TUs alongside the existing C consumer files.
- `dependency('Foundation', required: true)` + `dependency('Metal', required: true)` (was `required: false` in T8-1). Apple's frameworks are guaranteed present on macOS; the parent `subdir('metal')` gate already restricts this branch to Darwin hosts.
- `add_project_arguments(['-fobjc-arc', '-fno-objc-arc-exceptions', '-fobjc-weak'], language: 'objcpp')` so the Obj-C++ TUs compile under ARC. No `language: 'c'` carve-out is needed — meson dispatches Obj-C++ flags by file extension.

### Smoke-test expectations

[`core/test/test_metal_smoke.c`](../../core/test/test_metal_smoke.c) flips from the T8-1 `-ENOSYS` pin to runtime expectations:

- `vmaf_metal_state_init`, `vmaf_metal_context_new`, `vmaf_metal_kernel_lifecycle_init`, `vmaf_metal_kernel_buffer_alloc`: each returns `0` on Apple-Family-7+ devices, `-ENODEV` on every other host. The test gracefully short-circuits on `-ENODEV` rather than failing — keeps the test green on Intel-Mac CI lanes if any are ever added.
- `vmaf_metal_list_devices`, `vmaf_metal_device_count`: return a non-negative count (`0` is fine for non-Apple-7+).
- Input-validation paths (`NULL` arguments, non-zero `flags`) still fire unconditionally because they don't need a device.

The `motion_v2_metal` extractor stays at "registered but kernel not ready" — the first real kernel is T8-1c.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Objective-C++ via ARC + bridge casts (chosen) | Idiomatic, smallest cognitive distance to Apple's documentation; ARC removes manual `retain`/`release` bookkeeping; `__bridge_retained`/`__bridge_transfer` express +1/-1 ownership at the type system | Requires `-fobjc-arc` Obj-C++ flag, which means consumer TUs that include the runtime headers must be Obj-C++ or rely on opaque `void *` handles — handled by the accessor pair + uintptr_t ABI | Smallest blast radius. Header purity preserved; consumers stay pure-C. |
| MetalCpp (`<Metal/Metal.hpp>` single-header C++ wrapper) | Single-language story (pure C++), no Obj-C++ at all, NS::SharedPtr RAII | Apple's MetalCpp ships per Xcode release and isn't on every CI image; adds a vendored single-header dependency; some Metal entry points (`MTLCommandBufferStatus` callback) lag the Obj-C surface; community reports of leaks around `NS::SharedPtr` in pre-2024 versions | Adds a moving-target dependency to bottle through Homebrew; ARC pattern is well-understood and ships with every Apple Clang since Xcode 4.2 |
| Manual `retain`/`release` (no ARC) | No compiler-injected reference counting; cleanest with mixed C/Obj-C struct definitions | Manual ref-counting in code with bridge casts is error-prone, especially around exception unwinds and the kernel-template's two-event submit/finished pair | Trades a known-safe pattern for a known-foot-gun pattern |
| Skip Metal native, double-down on Vulkan-via-MoltenVK | Zero new code in libvmaf; works on Mac today | Pays MoltenVK translation cost on every dispatch forever; MoltenVK extension gaps already block one Vulkan feature path; Apple's roadmap is Metal, not Vulkan | Already shipping as the stopgap in `lusoris/homebrew-tap`; the strategic answer is native Metal |

## Consequences

- **Positive**:
  - The Metal backend's runtime contract goes from `-ENOSYS` to working. `vmaf_metal_state_init` allocates a real `MTLDevice` + `MTLCommandQueue`; `vmaf_metal_picture_alloc` returns a shared-storage `MTLBuffer`; the kernel-template lifecycle helpers create event pairs and drain command buffers correctly.
  - Unblocks T8-1c (first real kernel — `integer_motion_v2.metal`). The kernel author can rely on the lifecycle helpers without touching the runtime.
  - Provides a native path that will eventually replace Vulkan-via-MoltenVK in the [Lusoris Homebrew tap](https://github.com/lusoris/homebrew-tap), once T8-1c ships.
- **Negative**:
  - Three new `.mm` TUs add Obj-C++ build complexity. CI lane `Build — macOS Metal` already exists from T8-1; just needs the Apple Clang to be ≥ Xcode 14 (every GHA macos-latest qualifies).
  - The struct layout for `VmafMetalContext` (and `VmafMetalState`) now lives in `common.mm`, which means it's not visible to TUs that include `common.h`. Accessors above mitigate the loss; consumers that need raw struct introspection (debugger only) can read the runtime layout from the `.mm` source.
- **Neutral / follow-ups**:
  - T8-1c (first real kernel) is the immediate follow-up — tracked in [issue #763](https://github.com/VMAFx/vmafx/issues/763).
  - T8-1d through T8-1k (7 follow-up kernels) — mechanical replicas of T8-1c, one PR per kernel, ordered integer → float → SSIM (separable conv).
  - When T8-1c ships, the [Lusoris Homebrew tap `libvmaf` formula](https://github.com/lusoris/homebrew-tap/blob/master/Formula/libvmaf.rb) flips from `enable_vulkan=enabled` (MoltenVK stopgap) to `enable_metal=enabled`; MoltenVK deps demoted to `--with-moltenvk` opt-in.

## References

- [ADR-0361](0361-metal-compute-backend.md) — Metal compute backend scaffold (T8-1)
- [ADR-0212](0212-hip-backend-scaffold.md) — HIP backend scaffold (audit-first pattern)
- [ADR-0241](0241-hip-first-consumer-psnr.md) — HIP first kernel-template consumer (the structural twin)
- [ADR-0246](0246-cuda-kernel-template.md) — CUDA kernel template (origin of the lifecycle shape)
- [ADR-0338](0338-macos-vulkan-via-moltenvk-lane.md) — MoltenVK CI lane (the stopgap this PR will eventually retire)
- Issue [#763](https://github.com/VMAFx/vmafx/issues/763) — T8-1b + T8-1c tracking
- [Lusoris Homebrew tap](https://github.com/lusoris/homebrew-tap) — ships the MoltenVK stopgap; will swap to native Metal once T8-1c lands
- Source: `req` — paraphrased: contributor wanted native Metal acceleration on macOS rather than the MoltenVK stopgap ("I want metal, period").
