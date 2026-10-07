## VMAFx device frames and fences: shared contract on the CPU device (2026-10-07)

`rc4/api-wp3-common`, [ADR-1852](adr/1852-vmafx-api-redesign.md),
[ADR-1929](adr/1929-vmafx-device-frames-fences.md).

- `core/src/vmafx/` gains `device_context.c`, `fence.c`, `frame_import.c`,
  `frame_import_admit.c`, `frame_import_hooks.{c,h}` and `frame_pool.c`;
  `device.c` grows enumeration, information and the checks of
  `VmafxDeviceDesc`'s new `flags` and `external` fields. The backend lanes
  (CUDA, SYCL, HIP, Metal) add their device creation, memory kinds, fence
  kinds and per-extractor admission behind these functions.
- `frame_host.c`: the release of a non-pool frame is the shared
  `vmafx_frame_release()` and signals the frame's release fence after the
  last read; `vmafx_frame_read_desc()`, `vmafx_frame_host_device()` and
  `vmafx_frame_bind()` are shared with the import and the pool.
  `submit.c` checks admission before the engine counts a frame.
  `context.c` drops the context's device after a successful close, and reads
  and checks `VmafxContextConfig.import_retry_wait_ns` (ABI 0.1.3; the
  `vmaf_init` compat glue sets it to 0, the default).
  `error.c` keeps 1023 bytes of subject and message.
- `frame_import.c` includes `core/src/metal/iosurface_layout.h` for the
  NV12 / P010 / P016 row readers. A change to those readers changes the CPU
  import and the Metal import together (`test_vmafx_import_bitexact` and
  `test_metal_iosurface_layout`).
- `scripts/codegen/vmafx_api/ctext.py` spells an `out` string parameter
  `const char **` (it printed `const char * *`).
- `core/src/compat/gcc/stdatomic.h` (the fallback for a compiler without
  `<stdatomic.h>`, from dav1d) gains `atomic_uintptr_t`,
  `atomic_uint_fast64_t`, `atomic_store_explicit`, `atomic_exchange`,
  `atomic_compare_exchange_strong` / `_weak` and two memory orders, the C11
  atomics `core/src/vmafx/` uses. A re-sync of that file from dav1d keeps
  them.
- `core/test/vmafx_fixture_util.h` holds the fixture pairs and the reader
  both `test_vmafx_bitexact.c` and `test_vmafx_import_bitexact.c` use.
- No `libvmaf.h`, score, golden-data or FFmpeg patch impact.
