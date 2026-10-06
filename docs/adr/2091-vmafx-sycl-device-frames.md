<!-- markdownlint-disable MD013 MD060 -->
# ADR-2091: VMAFx device frames on SYCL: readers copy on the device behind the frame's ready event, the release waits on every reader

- **Status**: Accepted
- **Date**: 2026-10-06
- **Deciders**: maintainer (popup 2026-10-06); RC4 work package 3 (SYCL lane)
- **Tags**: api, rc4, gpu, sycl, opengl, zero-copy

## Context

[ADR-1929](1929-vmafx-device-frames-fences.md) fixed the shared contract of
zero-copy frame import and [ADR-2023](2023-vmafx-cuda-device-frames.md)
implemented it on CUDA. The SYCL lane of RC4 work package 3 implements it on
SYCL devices ([ADR-1829](1829-rc4-zero-copy-import.md) exit evidence): USM
pointer imports, Linux dma-buf imports with Intel-tiled layouts de-tiled on
the device ([ADR-1121](1121-sycl-qsv-zerocopy-p010-normalization.md)), the
chroma planes the luma-only zero-copy path of
[ADR-1688](1688-sycl-zero-copy-luma-only-admission.md) refuses, a SYCL event
barrier, `sync_file` fences on the dma-buf, and the OpenGL item of the work
package index (#2238: SYCL through an EGL dma-buf export, with a GL sync
acquire fence and the release callback the CUDA lane added).

The SYCL engine differs from the CUDA one in a way that decides the design:
no SYCL twin reads a picture where it is. The shared luma and chroma planes
(ADR-1369) are uploaded into slots the twins read, and every other twin packs
its own inputs on the host and uploads them. A frame in device memory has to
reach the same buffers without the host. Measurements on the lane's device
(an Arc A380 under the xe driver, DPC++ 2026.1.1 as `build-config.env` pins
it, Level Zero GPU driver 26.35; repeated from a first run with DPC++ 2026.0)
also ruled out the queue's pitched copy for the readers and barriers for the
ordering ([Research-2160](../research/2160-vmafx-sycl-device-frames.md)).

## Decision

We implement VMAFx device frames on SYCL with these rules.

1. **Devices are the Level Zero GPUs; one library queue per device.** A
   device opened by index is that Level Zero GPU (another backend's view of
   the same GPU is not counted twice); one opened from the caller's queue
   (`external[0]`, a `sycl::queue *`) lives in its context and device. The
   device owns one in-order library queue with immediate command lists
   (`immediate_command_list`, adopting the finding of draft PR #2217's
   ADR-1763 for the queue imports are made against; see item 10). A context
   on the device gets an engine state whose queues live in the device's SYCL
   context (`vmaf_sycl_state_init_queue()`), so the device's USM is valid in
   every queue the engine reads it on.
2. **Every reader copies on the device, through one helper.** A frame of the
   device is a SYCL device picture (`VMAF_PICTURE_BUFFER_TYPE_SYCL_DEVICE`,
   its private part naming the frame's runtime state). The shared luma and
   chroma uploads and every twin that stages its own planes (`float_psnr`,
   `float_motion`, `float_adm`, `float_vif`, `float_ssim` / `ssim`,
   `float_ms_ssim`, `ciede`, `ssimulacra2`, the SpEED pair, `motion`'s chroma)
   copy a device picture's plane with `vmaf_sycl_picture_read_plane()`: one
   kernel on the reader's queue that waits on the frame's ready event and is
   recorded as one of the frame's readers. `float_ms_ssim` converts level 0
   on the device with `picture_copy()`'s arithmetic (a division by a power of
   two, exact). No twin reads a device picture on the host; a CPU extractor
   that would is refused by admission and, should one appear, by the engine.
3. **Ordering by events.** A `SYCL_EVENT` acquire fence becomes a join on
   the library queue: an empty kernel that depends on the producer's event.
   The frame's conversions follow it there, and the frame's ready event is the
   last of them. The release, where the last reference is dropped, is one join
   on the library queue over the ready event and every recorded read, from
   whichever queue and context: a `HOST` release fence is signalled by a host
   task behind it, a `SYCL_EVENT` release fence is its event. A join is a
   kernel and not `ext_oneapi_submit_barrier()`, because a barrier on the
   event of a command that waits on a host task returns only once that host
   task ran (Research-2160 finding 2): a producer that holds its queue with a
   host task would make the import wait on the host. A SYCL event exists only once its command is submitted, so the
   library hands out a pointer to an event object of its own, kept in a table
   and answered "pending" by `vmafx_fence_wait()` until the release records
   into it (ADR-2023 item 3); the release callback runs after the recording.
4. **Producer memory is bound for any layout.** Readers copy rows, so a USM
   plane (device, shared or host USM of the device's context) and a linear
   dma-buf plane are bound at any address and pitch; a pointer that is no USM
   of the context is refused naming the plane. `VMAFX_IMPORT_ALLOW_COPY`
   changes nothing on SYCL: no layout needs a copy. NV12 / P010 / P016 are
   planarised on the device by integer kernels (a de-interleave and the P010
   shift, nothing else).
5. **dma-bufs.** Each plane's descriptor is duplicated and imported once
   through Level Zero external memory (the import of `dmabuf_import.cpp`, made
   on the library queue's context). Modifier 0 is bound; Intel Y-tiled and
   Tile4 planes are de-tiled on the device with the address math of
   `core/src/sycl/detile.h`, which the VA-surface import now shares (one
   definition); any other modifier is refused naming the plane. The imports
   are freed once the release join completed.
6. **Acquire fences the device cannot wait on are checked on the host.**
   `HOST`, `SYNC_FILE` and `GL_SYNC` acquire fences are passed when signalled
   and are `VMAFX_E_BUSY` otherwise, which the D8 helper waits on before its
   one retry. A dma-buf's implicit write fences
   (`DMA_BUF_IOCTL_EXPORT_SYNC_FILE`) are honoured the same way, and the D8
   helper waits on them too. A `SYNC_FILE` fence is polled on its descriptor
   and destroyed by closing it; the GL sync wait is shared with the CUDA lane
   (`core/src/vmafx/sync_object.c`).
7. **OpenGL through an EGL dma-buf export.** The texture of each plane becomes
   an EGL image of the current EGL context and is exported as one dma-buf
   (`EGL_MESA_image_dma_buf_export`), then imported as in item 5. The implicit
   fences the export leaves on the dma-buf (the GL driver's own work for the
   export) are waited on for at most 1 s. EGL entry points are resolved at
   run time.
8. **Pools.** A SYCL pool frame is device USM of the device's context. Its
   release records an idle event in a table slot, and its next acquire waits
   on it on the host. A pool frame carries no acquire fence and SYCL has no
   context-wide barrier the library could put in its place (the CUDA lane uses
   ADR-1199's): the caller finishes its writes before the submit.
9. **Not in this lane.** A `SYNC_FILE` release fence and the import of the
   release into the dma-buf (`DMA_BUF_IOCTL_IMPORT_SYNC_FILE`) need a kernel
   fence for Level Zero work, which this stack offers only through external
   semaphores; they are refused naming the kind. `WIN32_SHARED` memory is
   refused naming the field until a Windows device runs its tests.
10. **Immediate command lists, evaluated.** Draft PR #2217 measured, on an Arc
    A380 under i915, per-frame VA imports silently dropped under batched
    command lists unless the queue the imports are made against used
    immediate command lists. This lane's stream test (48 frames through 4
    reused VA surfaces, each imported and freed) scored bit for bit as host
    frames in 10 of 10 runs in each of three modes on the A380 under xe, with
    DPC++ 2026.1.1 and with 2026.0: default, batched with the library queue's
    property removed, and batched with it kept. The defect did not reproduce here; the library queue keeps
    the property as the cheap precaution #2217 recommends.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Readers copy device pictures on the device through one helper (chosen) | Every twin keeps its arithmetic and its own buffers; any producer layout binds; one place orders reads and records them for the release | One device copy per reader of a plane | Chosen |
| Twins read device pictures in place | No copy | Every twin's staging and its kernels rewritten for strided device inputs, per twin, per depth | Not chosen (a tuning item for RC8 if the copies show) |
| Copy into the shared slots with the queue's `memcpy()` | No kernel of our own | A plane with padded rows needs one `memcpy()` per row | Not chosen |
| `ext_oneapi_memcpy2d()` | Pitched copy in one call | Measured: one enqueued behind a host task lost the device, with DPC++ 2026.0 and 2026.1.1 (Research-2160 finding 1) | Rejected after measurement |
| One join over every reader at release (chosen) | Readers on any queue of any context; two contexts need nothing more | A list of events per frame; one empty kernel per acquire and per release | Chosen |
| `ext_oneapi_submit_barrier()` for the acquire and the release | The extension made for it | Measured: returns only once a host task ran when its wait list holds the event of a command behind that host task (Research-2160 finding 2) | Rejected after measurement |
| Read every frame on the library queue (the CUDA lane's rule) | One queue orders everything | The engine's twins run on their own queues; moving them all is a rewrite of the SYCL engine | Not chosen |
| `SYCL_EVENT` release event handed out at request time from a host task that waits for the release | A device wait is correct at any time | A host task blocked for each frame's lifetime holds a runtime thread per frame in flight | Not chosen |
| Host checks for `HOST` / `SYNC_FILE` / `GL_SYNC` (chosen) | Same rule on every device (ADR-1929 item 4) | A host wait when the producer is behind | Chosen |
| A host task on the library queue that waits on a `sync_file` | No caller stall | A runtime thread blocked per pending import; the import rule's retry already gives the caller the wait | Not chosen |
| Immediate command lists on the library queue (chosen) | #2217's precaution; no cost measured | — | Chosen |
| Batched command lists | The Arc A-series advice for kernels | #2217 measured dropped imports on i915 | Not chosen |

## Consequences

- **Positive** (measured with DPC++ 2026.1.1 in the dev image and with 2026.0
  on the host): imported SYCL frames score bit for bit as host-uploaded frames
  for every SYCL twin declared exact (the Netflix pair, both checkerboards,
  the 10-bit Sparks pair and the 4K pair, planar and semi-planar with padded rows at odd
  starts; `core/test/test_vmafx_import_sycl_bitexact.c`), from USM of every
  kind, linear dma-bufs, Tile4 VA surfaces and GL textures; a skipped acquire
  wait and an early release are seen under device load
  (`core/test/test_vmafx_import_sycl_fence.c`); chroma readers run on
  imported frames, which ADR-1688's zero-copy path refuses; the de-tile
  address math has one definition.
- **Negative**: every reader of a device picture copies the plane once on the
  device; `SYNC_FILE` release fences and `WIN32_SHARED` memory are refused;
  GL imports wait on the host for the EGL export's implicit fences (about
  2 ms measured); the caller finishes its writes to a pool frame before the
  submit; each acquire and each release costs one empty kernel launch.
- **Neutral / follow-ups**: `SYNC_FILE` release export through Level Zero
  external semaphores; the Windows shared-texture path with a Windows device;
  WP9's FFmpeg filter imports QSV / VA-API frames as dma-bufs (replacing
  `vmaf_sycl_import_va_surface()`'s luma-only path) and makes the decoder wait
  on the release from the release callback; the copies of item 2 are an RC8
  tuning row.

## References

- [ADR-1829](1829-rc4-zero-copy-import.md), [ADR-1852](1852-vmafx-api-redesign.md) design section 2.7, [ADR-1929](1929-vmafx-device-frames-fences.md), [ADR-2023](2023-vmafx-cuda-device-frames.md), [ADR-1897](1897-vmafx-abi-0x-numbering.md), [ADR-1121](1121-sycl-qsv-zerocopy-p010-normalization.md), [ADR-1688](1688-sycl-zero-copy-luma-only-admission.md), [ADR-1369](1369-sycl-shared-planes-light-twins.md), [ADR-1395](1395-sycl-kernels-no-scratch.md), [ADR-1468](1468-sycl-sub-group-sizes-every-aot-target.md).
- Draft PR #2217 (Tualua): ADR-1763, "the primary SYCL queue, which runs the VA-surface import, uses immediate command lists".
- [Research-2160](../research/2160-vmafx-sycl-device-frames.md): the runtime measurements behind items 2, 3 and 10.
- `Q` (maintainer popup 2026-10-06, ADR-2091): "Accept as designed (Recommended)", after the exit evidence was repeated with DPC++ 2026.1.1.
- `req` (RC4 work package 3, SYCL lane): "USM pointer; dma-buf + modifier (de-tile on device, ADR-1121); chroma (#2075); Windows shared texture | SYCL event barrier; `sync_file` export / import on the dma-buf | SYCL host device (scratch-free kernels, ADR-1395; AOT `--suite sycl-aot`)".
- `req` (RC4 work package index, added 2026-10-06, #2238): "OpenGL interop import (CUDA-GL, SYCL via EGL DMA-buf export, HIP-GL) and a GL sync fence kind (additive, ADR-1897)".
- Tests: `core/test/test_vmafx_import_sycl.c`, `test_vmafx_import_sycl_bitexact.c`, `test_vmafx_import_sycl_fence.c`, `test_vmafx_import_sycl_gl.c`, `test_vmafx_import_sycl_cells_contract.py`.
