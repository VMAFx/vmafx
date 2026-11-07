- An opt-in Vulkan integer compute path (ADR-3402), off by default
  (`-Denable_vulkan=enabled`): a device self-check probe with vendorID/deviceID
  selection, and the `adm2` integer feature on device classes the bit-exact parity
  gate has passed. The floating-point Vulkan backends and CI lanes removed by
  ADR-0726 stay removed.
