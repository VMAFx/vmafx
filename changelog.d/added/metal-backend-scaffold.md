- **Metal (Apple Silicon) compute backend scaffold (T8-1)
  ([ADR-0361](../docs/adr/0361-metal-compute-backend.md))**. Audit-first
  scaffold mirroring the HIP T7-10 split
  ([ADR-0212](../docs/adr/0212-hip-backend-scaffold.md)) and the
  original Vulkan T5-1 split
  ([ADR-0175](../docs/adr/0175-vulkan-backend-scaffold.md)). New public
  header [`libvmaf_metal.h`](../core/include/libvmaf/libvmaf_metal.h)
  declaring `VmafMetalState` / `VmafMetalConfiguration` /
  `vmaf_metal_state_init` / `_import_state` / `_state_free` /
  `vmaf_metal_list_devices` / `vmaf_metal_available`. New
  [`core/src/metal/`](../core/src/metal/) tree (common,
  picture_metal, dispatch_strategy, kernel_template, AGENTS.md) plus
  first-consumer kernel scaffold
  [`core/src/feature/metal/integer_motion_v2_metal.c`](../core/src/feature/metal/integer_motion_v2_metal.c)
  registering `vmaf_fex_integer_motion_v2_metal` (extractor name
  `motion_v2_metal`, `VMAF_FEATURE_EXTRACTOR_TEMPORAL` flag). All entry
  points return `-ENOSYS` until the runtime PR (T8-1b) lands. New
  `enable_metal` feature option (default **`auto`**: probes for
  `Metal.framework` / `MetalKit.framework` on macOS, disabled
  elsewhere). New 14-sub-test smoke at
  `core/test/test_metal_smoke.c` pinning the `-ENOSYS` contract for
  every public C-API entry point, the kernel-template helpers, and
  the first-consumer registration. New CI matrix row
  `Build — macOS Metal (T8-1 scaffold)` compiling on `macos-latest`
  with `-Denable_metal=enabled` (the macOS runner ships the Metal SDK
  as part of the system framework set; no extra install step needed).
  New operator-facing doc
  [`docs/backends/metal/index.md`](../docs/backends/metal/index.md)
  plus the index row in `docs/backends/index.md` flipped from "planned"
  to "scaffold". Apple Silicon (GPU Family Apple 7+) only — Intel Macs
  rejected per discontinued-platform reasoning. Runtime layer (T8-1b)
  will use Apple's official MetalCpp C++ wrapper
  (<https://developer.apple.com/metal/cpp/>, accessed 2026-05-09);
  MoltenVK passthrough rejected (translation overhead +
  double-dependency); Intel oneAPI rejected (no macOS distribution);
  OpenCL rejected (deprecated by Apple since macOS 10.14). User docs:
  [`docs/backends/metal/index.md`](../docs/backends/metal/index.md).
  Reproducer (on macOS): `meson setup build -Denable_metal=enabled
  && ninja -C build && meson test -C build test_metal_smoke`.
