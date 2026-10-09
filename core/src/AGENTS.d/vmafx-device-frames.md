---
paths:
  - core/src/vmafx/device*.c
  - core/src/vmafx/fence.c
  - core/src/vmafx/frame_import*.c
  - core/src/vmafx/frame_import_hooks.h
  - core/src/vmafx/frame_pool.c
  - core/src/vmafx/gl_sync.c
  - core/src/vmafx/sync_file.c
  - core/src/vmafx/release_events.c
  - core/src/vmafx/import_convert_kernels.h
  - core/src/compat/gcc/stdatomic.h
  - core/test/test_vmafx_import_*
  - core/test/vmafx_import_test_util.h
invariant: Import never host-copies; release fence at last picture ref; D8 wait per context, 10 s default.
---
<!-- markdownlint-disable MD013 -->
# VMAFx device frames, fences, pools (ADR-1929)

- Files: `device.c` (create, count, info, describe, profile), `device_context.c` (`vmafx_context_use_device`), `fence.c` (host fence objects, `vmafx_fence_*`), `frame_import.c` (import, release fence), `frame_import_admit.c` (admission, D8 helper), `frame_pool.c`, `frame_import_hooks.{c,h}` (test-only). Backend lanes add their kinds behind these functions; never a new function family per backend.
- Every `VmafxMemoryKind` / `VmafxFenceKind` declared; CPU lane: HOST memory, NONE / HOST fences. Other kinds -> `VMAFX_E_NOTSUP` naming kind until lane lands. `VmafxImportPlane` + `VmafxFence` embedded by value: frozen, never grow.
- Frame rule (ADR-1906 item 4) covers imported + pooled frames: release fence signalled where last picture count drops (`vmafx_frame_release()` / `pool_frame_release()`), any context or caller. `frame->released` = atomic `VmafxHostFence *`, set once (CAS); frame holds one ref, each handed-out fence one.
- Host fence: magic + `VmafRef` + atomic flag + mutex + condition variable. Signal sets flag under mutex and broadcasts; wait = timed condition wait in chunks of at most 1 s against monotonic deadline (Linux: cond on CLOCK_MONOTONIC; MSVC: Win32 shim's `pthread_cond_timedwait()`, relative timeout). Round count bounded as `vmafx_fence_poll()`; rounds used up -> poll for the rest. Virtual test clock -> poll path. Other fence kinds and `vmafx_window_wait()` poll through `vmafx_fence_poll()`. Poll (timeout 0) unsignalled -> `VMAFX_PENDING`, no error, no log; expired timeout -> `VMAFX_E_TIMEOUT`. Guard: `test_vmafx_host_fence_wait` (real clock, every platform).
- `VMAFX_IMPORT_ALLOW_COPY`: zero = zero copy (INIT zeroes all but `struct_size`). Never a host copy of device memory, flag or not.
- CPU import: planar unshifted planes bound (no copy); NV12 / P010 (>> 6) / P016 de-interleaved into one aligned `frame->owned` via `metal/iosurface_layout.h` row readers (one reference impl; change both together). Unsignalled HOST acquire -> `VMAFX_E_BUSY` (no queue on CPU).
- D8 = `vmafx_context_import_frame()`: import + `vmafx_context_admit()`; BUSY / TIMEOUT -> host wait on acquire (<= `context->import_retry_wait_ns`: `VmafxContextConfig.import_retry_wait_ns`, 0 -> `VMAFX_IMPORT_RETRY_WAIT_DEFAULT_NS` 10 s, > `VMAFX_IMPORT_RETRY_WAIT_MAX_NS` 600 s refused `VMAFX_E_RANGE`, never clamped; maintainer 2026-10-06) + one retry; else one failure naming input, backend, device, memory, format, bpc, size, modifiers, cause, attempts. `VmafxError` message 1023 bytes for it.
- Admission (`vmafx_admit_residency()`): host frame -> all admit; device frame -> CPU extractor refused (host copy), other backend refused, same backend = lane hook (today refused). Each refuser named. `vmafx_submit()`: both inputs same residency + device, then admission, before engine counts frame.
- `vmafx_context_use_device()`: once, before any feature / model / frame (twins picked at registration); context holds ref, dropped after successful close.
- Pool: `VmafRef` = caller + one per frame out; frame back at last count; destroy with frames out keeps them valid; exhaustion `VMAFX_E_BUSY`. Pool frames: priv + ref re-armed per acquire, cleared in release.
- C11 atomics used here (`atomic_uintptr_t`, `atomic_exchange`, CAS, `_explicit` orders) must exist in fallback `core/src/compat/gcc/stdatomic.h` too; new atomic op -> add it there.
- Test hooks (hidden symbols, static-lib tests only): counters `vmafx_count_host_copy()` (every host copy site calls it; lanes assert 0), `vmafx_count_conversion()`, import attempts; switches SKIP_ACQUIRE_WAIT, EARLY_RELEASE, FORCE_HOST_COPY; planted import status; residency override; virtual clock (`vmafx_test_set_virtual_clock()`: fence waits read it, each poll advances it by `VMAFX_FENCE_POLL_NS`, no sleep) + `vmafx_test_last_retry_wait_ns()` so a 10 s bound is tested without sleeping. Test that sets one clears it.
- Tests: `test_vmafx_import_api` (public), `test_vmafx_import_bitexact` (fixtures + synthetic 4K, NV12 / P010 / P016 == host frames, one import two contexts), `test_vmafx_import_fence` (Linux; acquire order, release canary incl. worker threads, host-copy counter, D8, admission). Each planted switch must make its test fail.
- Backend lanes plug in through `lane` / `lane_release` (`VmafxDevice`, `VmafxFrame`) and `lane_state` (`VmafxContext`); `vmafx_frame_release()` runs the lane's release before the release callback (`VmafxFrameImport.release`, ABI 0.1.7). CUDA lane: `core/src/cuda/vmafx_cuda.h`, invariants in `core/src/cuda/AGENTS.md` (ADR-2023). HIP lane: `core/src/hip/vmafx_hip.h`, `core/src/hip/AGENTS.d/vmafx-device-frames.md` (ADR-2092). Feature registration on a device context picks the device twin (`register.c`).
- Shared by lanes, one copy each: GL sync acquire + wait (`gl_sync.c`), sync_file poll (`sync_file.c`, Linux; `vmafx_fence_wait()` uses it for SYNC_FILE fences without a device), release-event table (`release_events.c`: event handed out pending until recorded at last ref; full table refuses), NV12 / P010 / P016 kernels (`import_convert_kernels.h`, compiled by nvcc and hipcc). `test_vmafx_fence_kinds` covers them on the CPU.
