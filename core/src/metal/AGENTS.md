# AGENTS.md — core/src/metal

Orientation for agents working on the Metal (Apple Silicon) backend.
Parent: [../../AGENTS.md](../../AGENTS.md). Mirrors
[`core/src/hip/AGENTS.md`](../hip/AGENTS.md) — the HIP and Metal
backends share an audit-first scaffold story, and Metal's `MTLDevice`
/ `MTLCommandQueue` / `MTLBuffer` API parallels HIP's `hipDevice_t`
/ `hipStream_t` / `hipMalloc` surface closely enough that the rebase
/ refactor contracts on this side track the HIP scaffold's.

## Scope

The Metal-side runtime (picture lifecycle, dispatch strategy, kernel
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
  compile with `-fobjc-arc`; Metal handles cross the Obj-C++ /
  pure-C boundary as `void *` / `uintptr_t`.
- `picture_metal.mm` — `vmaf_metal_picture_alloc` /
  `vmaf_metal_picture_free`. Allocates a `MTLBuffer` with
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
  `vmaf_metal_context_queue_handle()` expose the bridge-retained
  `void *` slots to consumer TUs (same pattern as
  `vmaf_hip_context_stream()`).
- Smoke test `test_metal_smoke.c` flipped from the T8-1 `-ENOSYS`
  pin to runtime expectations: on Apple-Family-7+ every entry
  point returns `0`; on every other host returns `-ENODEV`;
  input-validation paths still fire unconditionally.

**Batch-1 kernels landed** — T8-1c through T8-1j
([ADR-0421](../../../docs/adr/0421-metal-first-kernel-motion-v2.md)):

- `motion_v2_metal`, `float_psnr_metal`, `float_moment_metal`,
  `float_ansnr_metal`, `integer_psnr_metal`, `float_motion_metal`,
  `integer_motion_metal`, and `float_ssim_metal` are Obj-C++ host
  dispatch files backed by `.metal` shaders and the embedded
  `default.metallib`.
- `dispatch_strategy.c` is no longer inert. It answers support for
  both extractor names and provided feature keys for those landed
  kernels, and returns 0 for NULL contexts, NULL names, or unknown
  features.
- Remaining kernel ports (VIF, ADM, CIEDE, CAMBI, SSIMULACRA2, ...)
  follow as their own PRs gated by the `places=4` cross-backend-diff
  lane (per [ADR-0214](../../../docs/adr/0214-gpu-parity-ci-gate.md)).

## Ground rules

- **Parent rules** apply in full (see [../../AGENTS.md](../../AGENTS.md)).
- **Apple Silicon only** — the runtime PR (T8-1b) gates device
  selection on `MTLGPUFamily.Apple7` (M1 and later) via
  `-[id<MTLDevice> supportsFamily:]`. Intel Macs surface as
  `-ENODEV`. See ADR-0361 §"Apple Silicon-only" for the rationale
  (Apple's discontinuation of Intel-Mac GPU parity, plus
  unified-memory zero-copy is the load-bearing perf story).
- **Metal SDK is linked when `enable_metal=enabled`**. Since T8-1b
  (ADR-0420), `meson.build` declares `dependency('Foundation',
  required: true)` and `dependency('Metal', required: true)` (both
  gated behind the `is_metal_enabled` condition). Non-macOS hosts
  are unaffected: the Metal subdir is only entered when
  `host_machine.system() == 'darwin'` and the option is `enabled`
  or `auto` on macOS.
- **Metal runtime types cross headers as `uintptr_t`**. The public
  header `libvmaf_metal.h` and the kernel template's
  `VmafMetalKernelLifecycle` / `VmafMetalKernelBuffer` carry
  `uintptr_t` for `id<MTLCommandQueue>` / `id<MTLBuffer>` /
  `id<MTLEvent>` so consumer TUs stay free of `<Metal/Metal.h>` /
  `<Metal/Metal.hpp>`. Cast on the implementation side only.
  Mirrors the HIP ADR-0212 and Vulkan ADR-0184 patterns.
- **Don't drift the kernel-template field shape from HIP / CUDA
  beyond the documented unified-memory simplification**. The Metal
  template collapses the (device, pinned-host) pair into a single
  `MTLBuffer` with `MTLResourceStorageModeShared` because Apple
  Silicon has no separate device memory pool. The lifecycle struct
  (`cmd_queue` + `submit` + `finished` events) mirrors the HIP /
  CUDA twins field-for-field. See "Rebase-sensitive invariants"
  below.

## Rebase-sensitive invariants

- **`kernel_template.h` mirrors `hip/kernel_template.h` modulo the
  unified-memory buffer collapse** (fork-local, ADR-0361). The
  `VmafMetalKernelLifecycle` struct mirrors `VmafHipKernelLifecycle`
  field-for-field (one command-queue/stream slot + two event slots).
  The buffer struct `VmafMetalKernelBuffer` deliberately has one
  fewer slot than `VmafHipKernelReadback`: there is no
  `host_pinned` because Apple Silicon's unified memory makes
  `[buffer contents]` host-visible directly. Helper signatures
  (`vmaf_metal_kernel_lifecycle_init/_close`,
  `vmaf_metal_kernel_buffer_alloc/_free`,
  `vmaf_metal_kernel_submit_pre_launch`,
  `vmaf_metal_kernel_collect_wait`) parallel the HIP names with
  `_buffer_` substituted for `_readback_` to reflect the
  zero-copy posture. **On rebase / refactor**: if a fork PR touches
  `hip/kernel_template.h`'s lifecycle (e.g. adds a third event),
  walk the diff onto `metal/kernel_template.h` +
  `kernel_template.c` before merging. The buffer-vs-readback name
  asymmetry is intentional and stays.

- **`integer_motion_v2_metal.c` mirrors `integer_motion_v2_hip.c`
  call-graph-for-call-graph** (fork-local, ADR-0361). Same
  `MotionV2StateMetal` / `MotionV2StateHip` field shape modulo the
  ping-pong buffer slots being one (single `MTLBuffer` with
  `MTLResourceStorageModeShared`) rather than two, since unified
  memory means the previous-frame ref Y plane lives in the same
  address space the kernel reads. The `VMAF_FEATURE_EXTRACTOR_TEMPORAL`
  flag and the `flush()` callback contract stay aligned with the
  HIP / CUDA twins. **On rebase**: if a future PR drifts the HIP
  twin's lifecycle (e.g. adds a third buffer), update the Metal
  twin in the same PR.

- **`vmaf_fex_integer_motion_v2_metal` is registered without the
  `VMAF_FEATURE_EXTRACTOR_METAL` flag bit set** (fork-local,
  ADR-0361; unchanged through T8-1b). The flag bit is reserved in
  the enum; the consumer does not set it yet because the
  picture buffer-type check in
  `vmaf_feature_extractor_context_extract` would route a
  Metal-flagged extractor through a (not-yet-existing) Metal
  buffer-type branch. T8-1c adds the
  `VMAF_PICTURE_BUFFER_TYPE_METAL_DEVICE` tag and *then* sets the
  flag on the extractor. **On rebase**: if a refactor touches the
  picture buffer-type dispatch, leave the Metal extractor's flags
  at `VMAF_FEATURE_EXTRACTOR_TEMPORAL` only until T8-1c. Same
  rationale as the HIP twin's `VMAF_FEATURE_EXTRACTOR_HIP`-deferral
  posture.

- **Metal device-index numbering space is the filtered Apple7+ subset**
  (fork-local). `select_device_or_nil`, `vmaf_metal_device_count`, and
  `vmaf_metal_list_devices` (all in `common.mm`) MUST enumerate the *same*
  filtered Apple-Family-7+ subset of `MTLCopyAllDevices()`, so
  `--metal_device N` selects exactly the device printed as `[N]` by the
  device list. `select_device_or_nil` walks the array, skips non-Apple7
  devices, and returns the `N`-th surviving device — it does NOT index the
  raw `MTLCopyAllDevices()` array (a non-Apple7 device preceding an Apple7+
  one would otherwise cause an off-by-N or select a rejected device). **On
  rebase / refactor**: if a PR changes the device-family gate (e.g. raises
  the floor to Apple8) or the enumeration order, move all three functions
  together so the index space stays consistent.

- **`struct VmafMetalContext` layout is private to `common.mm`**
  (fork-local, ADR-0420). The struct definition lives in
  `common.mm`, not in `common.h`. Consumer TUs (`picture_metal.mm`,
  `kernel_template.mm`) reach the handles only through the
  `vmaf_metal_context_device_handle()` /
  `vmaf_metal_context_queue_handle()` accessor pair. **On rebase**:
  reject any diff that re-introduces a local struct-layout replica
  in a consumer or header.

- **Bridge-cast ownership discipline** (fork-local, ADR-0420). All
  `.mm` TUs use `-fobjc-arc`. Metal object handles are stashed into
  C `void *` slots with `(__bridge_retained void *)id` (+1 retain)
  and released back with `(__bridge_transfer id<...>)void *` (-1
  release). Borrows within a TU use `(__bridge id<...>)void *` (no
  refcount change) and are only valid while the C slot holds the +1.
  **On rebase**: audit every bridge cast against this pattern; a
  missing `_retained` leaks, a missing `_transfer` double-frees.

- **Kernel-template lifecycle mirrors HIP twin** (fork-local,
  ADR-0420). `kernel_template.mm` follows `hip/kernel_template.c`
  field-for-field modulo the unified-memory buffer collapse (single
  `MTLBuffer` + `MTLResourceStorageModeShared` vs. HIP's
  `(device, pinned-host)` pair). **On rebase**: if a fork PR grows
  the HIP twin's lifecycle (e.g. adds a third event slot or a new
  staging step), propagate the same change to `kernel_template.mm`
  in the same PR.

## Governing ADRs

- [ADR-0361](../../../docs/adr/0361-metal-compute-backend.md) —
  Metal scaffold-only audit-first PR (T8-1, this PR).
- [ADR-0212](../../../docs/adr/0212-hip-backend-scaffold.md) — HIP
  scaffold-only audit-first PR (T7-10); the precedent this PR
  mirrors.
- [ADR-0175](../../../docs/adr/0175-vulkan-backend-scaffold.md) —
  Vulkan scaffold (T5-1); the original audit-first GPU-backend
  precedent.
- [ADR-0246](../../../docs/adr/0246-gpu-kernel-template.md) — GPU
  kernel-template decision; the source the Metal mirror tracks
  (via the HIP twin that mirrors the CUDA twin).
- [ADR-0214](../../../docs/adr/0214-gpu-parity-ci-gate.md) —
  `places=4` cross-backend gate; the runtime PR's incoming
  numerics gate.
- [ADR-0145](../../../docs/adr/0145-motion-v2-neon-bitexact.md) —
  motion_v2 NEON twin on Apple Silicon CPU. Coordinates with this
  ADR: NEON stays the CPU-side path; Metal is the GPU-side path.

## Build

```bash
# On macOS:
meson setup build -Denable_metal=enabled
ninja -C build
meson test -C build test_metal_smoke

# On Linux / Windows: -Denable_metal=auto resolves to disabled
# (no Metal frameworks); -Denable_metal=enabled fails the meson
# setup with a clear missing-framework error from the dependency()
# probe.
```

The scaffold has zero hard runtime dependencies on non-macOS hosts.
On macOS the meson `dependency('Metal') / dependency('MetalKit')`
probes resolve to the system frameworks; `Build — macOS Metal
(T8-1 scaffold)` in `.github/workflows/libvmaf-build-matrix.yml`
runs this exact configuration on every PR.
