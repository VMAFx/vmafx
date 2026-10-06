---
paths:
  - core/src/hip/import_*.c
  - core/src/hip/import_convert.hip
  - core/src/hip/vmafx_hip.h
  - core/src/hip/vmafx_hip_internal.h
  - core/test/test_vmafx_import_hip*
  - core/test/vmafx_hip_test_util.h
invariant: Imported HIP frames are read only on the library stream; reader and null stream wait; import submits.
---
<!-- markdownlint-disable MD013 -->
# VMAFx device frames on HIP (ADR-2092)

- Lane API: `vmafx_hip.h` (called from `core/src/vmafx/*`), state `vmafx_hip_internal.h`. Files: `import_device.c` (count / info / open / close / context attach), `import_frame.c` (checks, plan, conversions, bind), `import_dmabuf.c`, `import_fence.c`, `import_gl.c`, `import_convert.hip` (kernels = `core/src/vmafx/import_convert_kernels.h`, shared with CUDA; change once).
- One library stream per device (`VmafxHipDevice.str`): own non-blocking stream, or caller's `hipStream_t` in `external[0]` (`external[1]` must be 0). Every import = `VMAF_PICTURE_BUFFER_TYPE_HIP_DEVICE` picture, `priv->hip.str` = that stream.
- Twins read host-picture style: device picture -> D2D copy on library stream + event; twin stream AND null stream wait (`vmaf_hip_stream_wait_library()`; `integer_adm_hip`, `psnr_hip`, `float_vif_hip` launch on null stream: 7 wrong ADM values frame 0 without). Never read producer memory on another stream: release on library stream must follow every read.
- Acquire: HIP_EVENT = `hipStreamWaitEvent` on library stream before copies (planted skip `VMAFX_TEST_SKIP_ACQUIRE_WAIT`). SYNC_FILE / GL_SYNC / HOST = host check, unsignalled -> BUSY, D8 waits (`core/src/vmafx/sync_file.c`, `gl_sync.c`). No device wait on sync_file: `hipImportExternalSemaphore()` aborts (ROCm 7.2.4, T-HIP-ROCM-EXTERNAL-SEMAPHORE-ABORT-2026-10-06).
- Release at last ref (`vmafx_hip_release_frame()`): HIP_EVENT recorded on library stream via shared table (`core/src/vmafx/release_events.c`; `hipEventQuery` on unrecorded event = success, so table answers pending), owned planes `hipFreeAsync`, dma-buf buried, HOST via `hipLaunchHostFunc`. Then release callback (common code). SYNC_FILE release refused NOTSUP, never faked.
- `hipStreamQuery(library)` right after `enqueue_import()` in `bind_hip_frame()`: gfx1036 let later copies overtake unsubmitted array read-outs (5/6 runs wrong). Keep (T-HIP-GFX1036-UNFLUSHED-STREAM-ORDER-2026-10-06).
- dma-buf: dup fd (`F_DUPFD_CLOEXEC`), import size = `lseek(SEEK_END)` (runtime checks nothing), map whole, plane = map + offset; producer `size` > actual -> RANGE; modifier != 0 / plane_index != 0 -> NOTSUP. Free behind event ("graves", reaped at import / close): `hipFree` syncs device (200 ms).
- GL: `check_gl_context()` (current GLX ctx, same PCI bus id) BEFORE any HIP-GL call: failed first setup crashes later calls (T-HIP-GL-INTEROP-SETUP-CRASH-2026-10-06). `<hip/hip_runtime_api.h>` before `<hip/hip_gl_interop.h>` (blank line keeps formatter order).
- No HIP frame pools (`frame_pool.c` refuses, T-HIP-VMAFX-NO-FRAME-POOLS-2026-10-06). No host copy anywhere; planted `VMAFX_TEST_FORCE_HOST_COPY` counted.
- Tests (device, flock `hip-gfx1036.lock`): `test_vmafx_import_hip` (API, layouts, arrays incl. P010, dma-buf, two contexts, callback), `_bitexact` (every `exact_twins.d/*.hip` cell == host upload; `VMAFX_TEST_CLIPS=n`), `_fence` (acquire / sync_file / release arms, planted switches), `_gl` (GLX). Platform drops (T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01): tests repeat a differing cell up to 4x and print it; a defect differs every attempt. Device-free: `test_vmafx_import_hip_contract.py`, `test_hip_shared_frame` + contract, `test_vmafx_fence_kinds`.
