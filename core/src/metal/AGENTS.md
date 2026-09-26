# AGENTS.md — core/src/metal

Orientation for agents working on Metal (Apple Silicon) backend.
Parent: [../../AGENTS.md](../../AGENTS.md). Mirrors
[`core/src/hip/AGENTS.md`](../hip/AGENTS.md) — HIP and Metal backends
share audit-first scaffold story. Metal's `MTLDevice` /
`MTLCommandQueue` / `MTLBuffer` API parallels HIP's `hipDevice_t` /
`hipStream_t` / `hipMalloc` surface closely enough that rebase /
refactor contracts on this side track HIP scaffold's.

## Scope

Metal-side runtime (picture lifecycle, dispatch strategy, kernel
scaffolding template). Metal **feature kernels** live one level
deeper in [../feature/metal/](../feature/metal/).

```text
metal/
  common.{mm,h}          # Metal context + command-queue management (T8-1b / ADR-0420)
  picture_metal.{mm,h}   # VmafPicture on a Metal device — MTLBuffer lifecycle (T8-1b)
  dispatch_strategy.{c,h} # Feature-name → landed-kernel support predicate
  kernel_template.{mm,h} # per-feature kernel scaffolding + runtime (T8-1b / ADR-0420)
  stubs.c                # -ENOSYS fallbacks for the public libvmaf_metal.h
                         #   entry points when HAVE_METAL is OFF. Wired in
                         #   from core/src/meson.build's `else` branch of
                         #   `if is_metal_enabled`, NOT via `subdir('metal')`.
                         #   Mirrors core/src/dnn/dnn_api.c's VMAF_HAVE_DNN
                         #   stub pattern.
  meson.build           # subdir() include from core/src/meson.build
```

## Backend status

**Runtime landed** — T8-1b
([ADR-0420](../../../docs/adr/0420-metal-backend-runtime-t8-1b.md)):

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
([ADR-0421](../../../docs/adr/0421-metal-first-kernel-motion-v2.md)):

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
  lane (per [ADR-0214](../../../docs/adr/0214-gpu-parity-ci-gate.md)).

## Ground rules

- **Parent rules** apply in full (see [../../AGENTS.md](../../AGENTS.md)).
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

## Rebase-sensitive invariants

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
  `hip/kernel_template.h`'s lifecycle (e.g. adds a third event),
  walk the diff onto `metal/kernel_template.h` +
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
  adds a third buffer), update Metal twin in same PR.

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

- **Metal device-index numbering space is filtered Apple7+ subset**
  (fork-local). `select_device_or_nil`, `vmaf_metal_device_count`,
  and `vmaf_metal_list_devices` (all in `common.mm`) MUST enumerate
  *same* filtered Apple-Family-7+ subset of `MTLCopyAllDevices()`,
  so `--metal_device N` selects exactly device printed as `[N]` by
  device list. `select_device_or_nil` walks array, skips non-Apple7
  devices, returns `N`-th surviving device: does NOT index raw
  `MTLCopyAllDevices()` array. Non-Apple7 device preceding Apple7+
  one would otherwise cause off-by-N or select rejected device.
  **On rebase / refactor**: if PR changes device-family gate (e.g.
  raises floor to Apple8) or enumeration order, move all three
  functions together so index space stays consistent.

- **`struct VmafMetalContext` layout is private to `common.mm`**
  (fork-local, ADR-0420). Struct definition lives in `common.mm`,
  not in `common.h`. Consumer TUs (`picture_metal.mm`,
  `kernel_template.mm`) reach handles only through
  `vmaf_metal_context_device_handle()` /
  `vmaf_metal_context_queue_handle()` accessor pair. **On rebase**:
  reject any diff that re-introduces local struct-layout replica in
  consumer or header.

- **Bridge-cast ownership discipline** (fork-local, ADR-0420). All
  `.mm` TUs use `-fobjc-arc`. Metal object handles stashed into C
  `void *` slots with `(__bridge_retained void *)id` (+1 retain),
  released back with `(__bridge_transfer id<...>)void *` (-1
  release). Borrows within a TU use `(__bridge id<...>)void *` (no
  refcount change), valid only while the C slot holds the +1. **On
  rebase**: audit every bridge cast against this pattern; a missing
  `_retained` leaks, a missing `_transfer` double-frees.

- **Kernel-template lifecycle mirrors HIP twin** (fork-local,
  ADR-0420). `kernel_template.mm` follows `hip/kernel_template.c`
  field-for-field modulo unified-memory buffer collapse (single
  `MTLBuffer` + `MTLResourceStorageModeShared` vs. HIP's
  `(device, pinned-host)` pair). **On rebase**: if a fork PR grows
  the HIP twin's lifecycle (e.g. adds a third event slot or a new
  staging step), propagate the same change to `kernel_template.mm`
  in the same PR.

## Governing ADRs

- [ADR-0361](../../../docs/adr/0361-metal-compute-backend.md) —
  Metal scaffold-only audit-first PR (T8-1, this PR).
- [ADR-0212](../../../docs/adr/0212-hip-backend-scaffold.md) — HIP
  scaffold-only audit-first PR (T7-10); precedent this PR mirrors.
- [ADR-0175](../../../docs/adr/0175-vulkan-backend-scaffold.md) —
  Vulkan scaffold (T5-1); original audit-first GPU-backend
  precedent.
- [ADR-0246](../../../docs/adr/0246-gpu-kernel-template.md) — GPU
  kernel-template decision; source Metal mirror tracks (via HIP
  twin that mirrors CUDA twin).
- [ADR-0214](../../../docs/adr/0214-gpu-parity-ci-gate.md) —
  `places=4` cross-backend gate; runtime PR's incoming numerics
  gate.
- [ADR-0145](../../../docs/adr/0145-motion-v2-neon-bitexact.md) —
  motion_v2 NEON twin on Apple Silicon CPU. Coordinates with this
  ADR: NEON stays CPU-side path; Metal is GPU-side path.

## Build

```bash
# On macOS:
meson setup build -Denable_metal=enabled
ninja -C build
python3 "$(git rev-parse --show-toplevel)/scripts/ci/run_meson_test.py" -- -C build test_metal_smoke

# On Linux / Windows: -Denable_metal=auto resolves to disabled
# (no Metal frameworks); -Denable_metal=enabled fails the meson
# setup with a clear missing-framework error from the dependency()
# probe.
```

Scaffold has zero hard runtime dependencies on non-macOS hosts. On
macOS meson `dependency('Metal') / dependency('MetalKit')` probes
resolve to system frameworks; `Build — macOS Metal
(T8-1 scaffold)` in `.github/workflows/libvmaf-build-matrix.yml`
runs this exact configuration on every PR.
