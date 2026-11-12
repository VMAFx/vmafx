---
paths:
  - core/src/metal/common.h
  - core/src/metal/common.mm
  - core/src/metal/state_priv.h
  - core/src/metal/picture_import.mm
  - core/src/metal/picture_metal.mm
  - core/src/metal/kernel_template.mm
invariant: Filtered Apple7+ device numbering, state_priv.h VmafMetalContext layout, and ARC bridge-cast ownership.
---
# Device numbering, private context, and ARC bridge casts

## Rebase-sensitive invariants

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

- **`struct VmafMetalContext` layout lives in private `state_priv.h`, never `common.h`**
  (fork-local, ADR-0420, ADR-2091). `core/src/metal/state_priv.h` defines
  `struct VmafMetalContext` (`device_index`, `void *device`,
  `void *command_queue`) and `struct VmafMetalState` with plain C types (no
  Objective-C headers), included only by `common.mm` and `picture_import.mm`.
  `common.h` keeps opaque `typedef struct VmafMetalContext VmafMetalContext;`;
  consumer TUs (`picture_metal.mm`, `kernel_template.mm`, `feature/metal/*.mm`)
  reach handles only through `vmaf_metal_context_device_handle()` /
  `vmaf_metal_context_queue_handle()` accessor pair. **On rebase**: keep
  `common.h` opaque, keep `state_priv.h` free of Objective-C types, and reject
  duplicate struct-layout definitions in `common.mm` or consumer TUs.

- **Bridge-cast ownership discipline** (fork-local, ADR-0420). All
  `.mm` TUs use `-fobjc-arc`. Metal object handles stashed into C
  `void *` slots with `(__bridge_retained void *)id` (+1 retain),
  released back with `(__bridge_transfer id<...>)void *` (-1
  release). Borrows within TU use `(__bridge id<...>)void *` (no
  refcount change), valid only while C slot holds +1. **On
  rebase**: audit every bridge cast against this pattern; omitting
  `_retained` lets ARC release object while C slot still references it,
  and omitting `_transfer` leaks retained object.

- **Kernel-template lifecycle mirrors HIP twin** (fork-local,
  ADR-0420). `kernel_template.mm` follows `hip/kernel_template.c`
  field-for-field modulo unified-memory buffer collapse (single
  `MTLBuffer` + `MTLResourceStorageModeShared` vs. HIP's
  `(device, pinned-host)` pair). **On rebase**: if a fork PR grows
  the HIP twin's lifecycle (e.g. adds third event slot or a new
  staging step), propagate the same change to `kernel_template.mm`
  in the same PR.
