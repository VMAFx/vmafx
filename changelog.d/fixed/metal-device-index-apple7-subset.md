### Fixed

- **Metal `--metal_device N` selected the wrong GPU** (`core/src/metal/common.mm`):
  `select_device_or_nil` indexed the raw `MTLCopyAllDevices()` array, but
  `vmaf_metal_device_count` / `vmaf_metal_list_devices` enumerate the *filtered
  Apple-Family-7+ subset*. When a non-Apple7 device precedes an Apple7+ one,
  `[N]` in the device list ≠ the device selected. Fix: index into the same
  filtered Apple7+ subset, so `--metal_device N` matches the listed `[N]`.
  Invariant recorded in `core/src/metal/AGENTS.md`.
