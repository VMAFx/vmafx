<!-- markdownlint-disable MD013 -->
# Research-2160: SYCL device frames: what the Level Zero runtime does with cross-queue copies, host tasks and event queries, and whether batched command lists drop imports on xe

- **Status**: Active
- **Workstream**: [ADR-2091](../adr/2091-vmafx-sycl-device-frames.md), [ADR-1929](../adr/1929-vmafx-device-frames-fences.md), [ADR-2023](../adr/2023-vmafx-cuda-device-frames.md)
- **Last updated**: 2026-10-06

## Question

A VMAFx device frame on SYCL is written by a producer on its own queue,
read by every SYCL twin on the engine's queues, and released to the
producer once the last reader ran. Which SYCL primitives order those steps
on the device without a host wait, and which ones the runtime turns into
host waits, slow paths or device faults? And does the immediate command
list finding of draft PR #2217 (ADR-1763 in that branch: an Arc A380 under
i915 dropped per-frame VA imports under batched command lists) hold for
this lane?

## Sources

- Measured on one Intel Arc A380 (dg2-g11) under the Linux xe driver, every
  run under `sycl-a380.lock`, with two toolchains:
  - **DPC++ 2026.1.1** (`2026.1.1.20260724`, the release `build-config.env`
    pins as `ONEAPI_VERSION` 2026.1), in throwaway containers of the dev image
    `vmaf-dev-mcp:local` (Ubuntu 26.04.1; Level Zero GPU driver
    `libze-intel-gpu1` 26.35.39758.10, loader 1.34.0, IGC 2.41.5, Mesa
    26.0.8, iHD 26.1.2) with `/dev/dri` passed through. The numbers below are
    these.
  - **DPC++ 2026.0** (compiler 20260331) on the host (compute runtime
    26.35.39758, loader 1.32), where the lane was first measured; its numbers
    are in the footnotes.
- Stand-alone SYCL reproducers (one file each, no VMAFx code), and the lane's
  tests: `core/test/test_vmafx_import_sycl.c`,
  `test_vmafx_import_sycl_fence.c`, `test_vmafx_import_sycl_gl.c`.
- [`sycl_ext_oneapi_memcpy2d`](https://github.com/intel/llvm/blob/sycl/sycl/doc/extensions/supported/sycl_ext_oneapi_memcpy2d.asciidoc),
  [`sycl_ext_oneapi_enqueue_barrier`](https://github.com/intel/llvm/blob/sycl/sycl/doc/extensions/supported/sycl_ext_oneapi_enqueue_barrier.asciidoc),
  [`sycl_ext_oneapi_in_order_queue_events`](https://github.com/intel/llvm/blob/sycl/sycl/doc/extensions/supported/sycl_ext_oneapi_in_order_queue_events.asciidoc).
- Draft PR #2217 (Tualua), its ADR-1763.

## Findings

1. **`ext_oneapi_memcpy2d()` behind a host task loses the device.** An
   in-order queue runs a host task (6 ms sleep) and then a one-row
   `ext_oneapi_memcpy2d()` of 3 MiB; the first iteration ends with
   `UR_RESULT_ERROR_DEVICE_LOST`, and the kernel log shows
   `xe 0000:03:00.0: [drm] exec queue reset detected`. The same program with
   `memcpy()` in place of the 2D copy runs all iterations. A Level Zero trace
   (`UR_L0_DEBUG=1`) of one device-to-device 2D copy of a padded 1920x1080
   plane shows a single kernel launch (`zeCommandListAppendLaunchKernel`).
   The library copies pitched planes with a kernel of its own (`copy_rows()`
   in `core/src/sycl/vmafx_sycl_rt.cpp`), and the test producer copies packed
   planes with one `memcpy()` and padded ones with one `memcpy()` per
   row.[^f1]
2. **A barrier on the event of a command behind a host task waits on the
   host; a kernel with the same dependency does not.** A producer queue runs a
   50 ms host task and then a 1 MiB `memcpy()`; on a second queue,
   `ext_oneapi_submit_barrier({that copy's event})` returned after 50.21 to
   51.15 ms, in all four combinations of batched and immediate command lists
   for the barrier's queue and a reader queue behind it. A kernel or a
   `memcpy()` with `depends_on()` on the event of a command behind a host task
   returned after 0.03 to 0.07 ms and ran after the host task. With the
   producer held by a device kernel of about 125 ms instead of a host task,
   the barrier returned after 0.06 to 0.24 ms and a reader `memcpy()` behind
   it after 0.07 to 0.62 ms. A host task still pending on a queue (the `HOST`
   release fence's) delays no later submission to that queue (0.07 to
   0.62 ms).[^f2] The library
   therefore orders the acquire and the release with an empty kernel that
   depends on the events (`join()` in `core/src/sycl/vmafx_sycl_rt.cpp`), not
   with a barrier, so neither the import nor the release waits on the host
   for a producer that holds its queue with a host task.
3. **`ext_oneapi_get_last_event()` waits for the queue.** On an in-order
   queue whose last command is a 50 ms kernel, the call returned after
   51.7 to 54.2 ms in three runs (97.5 ms in the first, which built the
   kernel), at the kernel's completion; the submit itself took 0.05 ms. The
   test producer keeps its last event itself.[^f3]
4. **A device-side release canary did not see an early release.** A canary
   kernel the producer queued behind the release fence never ran before the
   readers, even with the planted early release (the fence opened when the
   submit returns): the device ran it after the readers whatever the fence
   said, as if it executed the kernels of every queue in submission order
   (inferred from this result, not measured directly). The release test
   writes its canary from the host, in the release callback, after a host
   wait on the release fence, into shared USM the readers' frames come from;
   with the planted early release it then sees 15 bad scores and 16 early
   canaries of 16 frames, and 0 of either with the real release (the same
   with 2026.1.1 and 2026.0).
5. **Long single work-item kernels reset the engine.** A one work-item spin
   of about one second (20 million iterations) ended in
   `UR_RESULT_ERROR_DEVICE_LOST` (measured with 2026.0, not repeated); the
   fence tests keep device holds at 30 ms or less.
6. **An EGL dma-buf export leaves pending implicit fences.** Right after
   `eglExportDMABUFImageMESA()` the dma-buf carried a write fence for about
   1.7 ms (the GL driver's work for the export; measured on the host's Mesa
   with 2026.0, and the GL test passes in the dev image). A non-blocking check refused
   the import as busy; the GL path waits up to 1 s for those fences, a
   dma-buf import checks without waiting and returns `VMAFX_E_BUSY`, which the
   import rule's helper waits on. When a texture is deleted after its exported
   descriptors were closed, Mesa prints
   `DMA_BUF_IOCTL_EXPORT_SYNC_FILE ioctl failed (9)`: a message of the GL
   driver about descriptors it no longer holds, with no effect on the import.
7. **Batched command lists did not drop VA imports on xe.** The stream test
   (48 frames of the Netflix pair through 4 reused NV12 VA surfaces, each
   exported, imported, scored by `psnr_sycl` and `cambi_sycl` and released)
   was run 10 times in each of three modes: the default (immediate command
   lists, the library queue's property set), batched command lists
   (`UR_L0_USE_IMMEDIATE_COMMANDLISTS=0`) with the property removed, and
   batched with the property kept. All 30 runs scored bit for bit as the
   host frames (192 values, 0 differing, each run), with 2026.1.1 and with
   2026.0. PR #2217's defect was measured on i915; this host runs xe, where it
   did not reproduce. The library queue keeps the property, the precaution
   #2217 recommends.
8. **A host wait on a queue while one of its host tasks completes can crash
   the runtime.** The acquire test's producer used to zero each frame buffer,
   wait on its queue (`queue::wait()`), then hold the queue with a host task
   and copy the frame, while imports on the library queue depended on the
   copies. In 8 of about 110 runs the process died with `SIGSEGV`, all tests
   having passed in some of them: 2 of 30 and 2 of 40 runs with DPC++ 2026.0,
   3 of 30 and 1 of 9 with 2026.1.1. The cores show the runtime's host-task
   worker thread in `Scheduler::NotifyHostTaskCompletion()` →
   `cleanupCommands()` → `Command::~Command()` →
   `event_impl::cleanDepEventsThroughOneLevel()` →
   `cleanupDependencyEvents()`, with the main thread in
   `Scheduler::waitForEvent()` under `queue::wait()` of the same producer
   queue (2026.0); with 2026.1.1 the same `cleanupDependencyEvents()` frame,
   and once `KernelProgramCache::reset()` doing a `memset()` on a null table at
   exit. No VMAFx frame is on any of these stacks, and making the acquire a
   barrier again did not change the rate (2 of 40). At process exit the
   library holds no release slot and no queue (checked under a debugger).
   With every buffer zeroed and finished before the first host task, so that
   the host never waits on the producer while one of its host tasks
   completes, the test ran 40 of 40 times clean with each toolchain and still
   sees a skipped acquire wait (16 bad of 16). A plain-SYCL loop of the same
   shape (memset, wait, host task, copy, a dependent kernel on a second queue,
   with and without a busy second thread) did not crash in 10 runs of 400
   iterations each, so no reproducer without VMAFx exists yet.

The runtime findings by toolchain (1 to 3 from plain-SYCL reproducers without
VMAFx code, 8 from the fence test); none is reported upstream yet:

| Finding | DPC++ 2026.0 (host) | DPC++ 2026.1.1 (dev image) |
| --- | --- | --- |
| 1. one-row `ext_oneapi_memcpy2d()` behind a host task | `UR_RESULT_ERROR_DEVICE_LOST`, xe exec queue reset | the same |
| 2. barrier on a command behind a 50 ms host task | returns after 50.2 to 51.6 ms | returns after 50.2 to 51.2 ms |
| 2. kernel / `memcpy()` with `depends_on()` on the same | returns after 0.03 ms | returns after 0.03 to 0.07 ms |
| 3. `ext_oneapi_get_last_event()` after a 50 ms kernel | returns after 50.6 to 84.5 ms | returns after 51.7 to 54.2 ms (97.5 ms with the kernel build) |
| 8. host wait on a queue while its host task completes (in the fence test) | `SIGSEGV` in host-task cleanup, 4 of 70 runs | `SIGSEGV` in host-task cleanup or at exit, 4 of 39 runs |

[^f1]: With DPC++ 2026.0 the 2D copy behind a host task lost the device the
    same way. An earlier version of this digest said that the 2D copy was one
    command per row on 2026.0; that was not traced (with `UR_L0_DEBUG=1` the
    2026.0 stack ends in a segmentation fault) and is withdrawn.
[^f2]: DPC++ 2026.0: barrier 50.2 to 51.6 ms; dependent kernel or `memcpy()`
    0.03 ms; with a 120 ms device hold, barrier 0.04 to 0.34 ms and reader
    `memcpy()` 0.06 to 1.02 ms; a pending host task on the queue delayed the
    next submission by 0.04 to 0.58 ms.
[^f3]: DPC++ 2026.0: 50.6 to 84.5 ms in four runs, submit 0.04 ms.

## Alternatives explored

- **Queue copies for readers**: `ext_oneapi_memcpy2d()` is rejected by
  finding 1, and a plane with padded rows needs one `memcpy()` per row; the
  readers copy with one kernel (`copy_rows()`).
- **`ext_oneapi_submit_barrier()` for the acquire and the release**: rejected
  by finding 2.
- **A device-side release canary**: rejected by finding 4.
- **A host task as the hold in the release test**: while the library ordered
  with barriers, the import waited on the host for the hold (finding 2), so
  the readers never overlapped the producer's next write. A calibrated device
  spin of at most 30 ms (finding 5) replaced it, which is also closer to a
  decoder's GPU work. The acquire test keeps a 6 ms host task before the
  producer's copy: a skipped acquire wait is seen (16 bad of 16) and the real
  wait gives 0 bad of 16.

## Open questions

- Findings 1 to 3 reproduce in plain SYCL with DPC++ 2026.0 and 2026.1.1
  (table above); they are candidates for an upstream report against the
  DPC++ runtime, not filed for now. Finding 8 has no plain-SYCL reproducer
  yet.
- The library signals a `HOST` release fence with a host task on its queue
  and waits on that queue when a device is closed, the shape of finding 8.
  No crash was seen there (the fence and import tests ran 40 and 20 times
  clean); a library thread that waits on the release events would remove
  host tasks from the library altogether if it ever appears.
- A `SYNC_FILE` release fence needs a kernel fence for Level Zero work
  (external semaphores); not evaluated.
- The behaviour under i915, and on Xe2 devices, is unmeasured in this lane.

## Related

- ADRs: [ADR-2091](../adr/2091-vmafx-sycl-device-frames.md), [ADR-1929](../adr/1929-vmafx-device-frames-fences.md), [ADR-2023](../adr/2023-vmafx-cuda-device-frames.md), [ADR-1395](../adr/1395-sycl-kernels-no-scratch.md), [ADR-1121](../adr/1121-sycl-qsv-zerocopy-p010-normalization.md)
- PRs: #2217 (draft, Tualua), #2213, #2277
