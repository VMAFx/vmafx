<!-- markdownlint-disable MD013 -->
# Research-2159: SYCL device frames: what the Level Zero runtime does with cross-queue copies, host tasks and event queries, and whether batched command lists drop imports on xe

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

- Measured on one host: Intel Arc A380 (dg2-g11), Linux xe driver,
  DPC++ 2026.0 (compiler 20260331), Level Zero compute runtime 26.35.39758,
  Level Zero loader 1.32, Mesa EGL. Every run under `sycl-a380.lock`.
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
   `memcpy()` in place of the 2D copy runs all iterations. With an image's
   rows, the 2D copy is also one command per row on Level Zero. The library
   therefore copies pitched planes with a kernel of its own
   (`copy_rows()` in `core/src/sycl/vmafx_sycl_rt.cpp`), and the test
   producer copies packed planes with one `memcpy()` and padded ones with one
   `memcpy()` per row.
2. **A barrier on the event of a command behind a host task waits on the
   host; a kernel with the same dependency does not.** A producer queue runs a
   50 ms host task and then a 1 MiB `memcpy()`; on a second queue,
   `ext_oneapi_submit_barrier({that copy's event})` returned after 50.2 to
   51.6 ms, in all four combinations of batched and immediate command lists
   for the barrier's queue and a reader queue behind it. A kernel or a
   `memcpy()` with `depends_on()` on the event of a command behind a host task
   returned after 0.03 ms and ran after the host task. With the producer held
   by a 120 ms device kernel instead of a host task, the barrier returned
   after 0.04 to 0.34 ms and a reader `memcpy()` behind it after 0.06 to
   1.02 ms. A host task still pending on a queue (the `HOST` release fence's)
   delays no later submission to that queue (0.04 to 0.58 ms). The library
   therefore orders the acquire and the release with an empty kernel that
   depends on the events (`join()` in `core/src/sycl/vmafx_sycl_rt.cpp`), not
   with a barrier, so neither the import nor the release waits on the host
   for a producer that holds its queue with a host task.
3. **`ext_oneapi_get_last_event()` waits for the queue.** On an in-order
   queue whose last command is a 50 ms kernel, the call returned after
   50.6 to 84.5 ms in four runs, at the kernel's completion; the submit
   itself took 0.04 ms. The test producer keeps its last event itself.
4. **A device-side release canary did not see an early release.** A canary
   kernel the producer queued behind the release fence never ran before the
   readers, even with the planted early release (the fence opened when the
   submit returns): the device ran it after the readers whatever the fence
   said, as if it executed the kernels of every queue in submission order
   (inferred from this result, not measured directly). The release test
   writes its canary from the host, in the release callback, after a host
   wait on the release fence, into shared USM the readers' frames come from;
   with the planted early release it then sees 15 bad scores and 16 early
   canaries of 16 frames, and 0 of either with the real release.
5. **Long single work-item kernels reset the engine.** A one work-item spin
   of about one second (20 million iterations) ended in
   `UR_RESULT_ERROR_DEVICE_LOST`; the fence tests keep device holds at 30 ms
   or less.
6. **An EGL dma-buf export leaves pending implicit fences.** Right after
   `eglExportDMABUFImageMESA()` the dma-buf carried a write fence for about
   1.7 ms (the GL driver's work for the export). A non-blocking check refused
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
   host frames (192 values, 0 differing, each run). PR #2217's defect was
   measured on i915; this host runs xe, where it did not reproduce. The
   library queue keeps the property, the precaution #2217 recommends.

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

- Findings 1 to 3 are reproducible in plain SYCL and are candidates for an
  upstream report against the DPC++ runtime; none is filed yet.
- A `SYNC_FILE` release fence needs a kernel fence for Level Zero work
  (external semaphores); not evaluated.
- The behaviour under i915, and on Xe2 devices, is unmeasured in this lane.

## Related

- ADRs: [ADR-2091](../adr/2091-vmafx-sycl-device-frames.md), [ADR-1929](../adr/1929-vmafx-device-frames-fences.md), [ADR-2023](../adr/2023-vmafx-cuda-device-frames.md), [ADR-1395](../adr/1395-sycl-kernels-no-scratch.md), [ADR-1121](../adr/1121-sycl-qsv-zerocopy-p010-normalization.md)
- PRs: #2217 (draft, Tualua), #2213, #2277
