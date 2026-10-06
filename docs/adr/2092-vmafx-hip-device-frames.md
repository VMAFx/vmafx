<!-- markdownlint-disable MD013 MD060 -->
# ADR-2092: VMAFx device frames on HIP: one library stream per device copies every frame for the twins, dma-bufs as external memory, sync_file checked on the host

- **Status**: Proposed
- **Date**: 2026-10-06
- **Deciders**: RC4 work package 3 (HIP lane); maintainer review on the draft PR
- **Tags**: api, rc4, gpu, hip, opengl, zero-copy

## Context

[ADR-1929](1929-vmafx-device-frames-fences.md) fixed the shared contract of
zero-copy frame import and [ADR-2023](2023-vmafx-cuda-device-frames.md)
implemented it on CUDA. The HIP lane of RC4 work package 3
([ADR-1829](1829-rc4-zero-copy-import.md) exit evidence) asks for device
pointers, dma-bufs imported as external memory (a path HIP did not have),
HIP event and sync_file fences, and the HIP-GL import with a GL sync acquire
fence that ADR-2023 added as kinds. Until now every HIP frame was uploaded
from the host (`T-HIP-NO-IMPORT-PATH-2026-10-05`).

The HIP twins differ from the CUDA ones in one way that shapes the design:
they read host pictures. Each twin (or the shared frame of
[ADR-1408](1408-hip-shared-frame-planes.md)) uploads the planes into device
buffers it owns, on its own stream, and three twins (`integer_adm_hip`,
`psnr_hip`, `float_vif_hip`) launch their kernels on the null stream. Three
more read planes on the host: `psnr_hvs_hip` and `ssimulacra2_hip` stage
them, and `float_ms_ssim_hip` builds level 0 with `picture_copy()`. There is
no HIP device picture the twins could be handed.

The platform the lane is measured on (gfx1036 iGPU, ROCm 7.2.4, Linux
7.2.9-1-cachyos) behaves differently from the CUDA host in ways that ruled
out the obvious designs; each was measured before it was decided
([Research-2159](../research/2159-vmafx-hip-device-frames.md)):

- The runtime has no sync_file semaphore type, and the types that could carry
  one fail: `hipImportExternalSemaphore()` of an opaque descriptor (a DRM
  syncobj) aborts the process (`rocdevice.hpp:281`,
  `NullDevice::importExtSemaphore`, `ShouldNotReachHere()`), and a timeline
  semaphore descriptor is `hipErrorInvalidValue`. A sync_file can therefore
  neither become a device-side wait nor be signalled from a stream.
- `hipImportExternalMemory()` neither checks the size it is given against the
  dma-buf nor takes ownership of the descriptor.
- `hipFree()` of a mapping synchronises the device: 200 ms behind a busy
  stream.
- The HIP-GL interop reads only the current GLX context; after its first
  setup failed (no GLX context, or a context of another GPU) later calls
  crash the process, and `hipGLGetDevices()` crashes outright under another
  vendor's GLX context.
- `hipEventQuery()` on an event never recorded returns `hipSuccess`.
- Work left unsubmitted on one stream (an array read-out) was overtaken by
  copies enqueued later on another stream that waits for it: the last frame
  of a planar array clip was wrong in 5 of 6 runs.
- A run of a stream's commands is sometimes never executed
  (`T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01`), with or without the
  import path.

## Decision

We implement VMAFx device frames on HIP with these rules.

1. **One library stream per device.** A HIP device opened by index creates a
   non-blocking stream on it; one made from the caller's objects uses the
   caller's stream (`external[0]`, the device is the stream's, `external[1]`
   must be 0). Every frame of the device is a HIP device picture
   (`VMAF_PICTURE_BUFFER_TYPE_HIP_DEVICE`) that carries that stream.
2. **The twins' upload becomes a device copy on the library stream.** Where a
   HIP twin uploaded a host picture, it now enqueues a device-to-device copy
   of a device picture on the picture's library stream
   (`vmaf_hip_picture_upload()`, the shared frame's batch), records an event
   there, and makes both its own stream and the null stream wait for it
   (`vmaf_hip_stream_wait_library()`). The three twins that read planes on
   the host read device pictures on the device: `psnr_hvs_hip` and
   `ssimulacra2_hip` copy them into their device buffers, and
   `float_ms_ssim_hip` builds level 0 with a conversion kernel of the same
   arithmetic as `picture_copy()`. Every read of a frame's memory is thus on
   the library stream, and work enqueued there after the last read runs
   after it. No plane of a device frame is copied to or from the host.
3. **Acquire fences.** A `HIP_EVENT` acquire fence is a
   `hipStreamWaitEvent()` on the library stream ahead of the import's copies
   and conversions. A `SYNC_FILE` acquire fence and a `GL_SYNC` fence (GL
   imports only) are checked on the host at import (`poll()`,
   `glClientWaitSync()`); unsignalled, the import is `VMAFX_E_BUSY` and the
   D8 helper (`vmafx_context_import_frame()`) waits and retries once. A
   `HOST` acquire fence is passed when signalled and is `VMAFX_E_BUSY`
   otherwise, as on the other devices. The shared host-side parts are
   `core/src/vmafx/sync_file.c` and `core/src/vmafx/gl_sync.c` (the GL loader
   moved there from the CUDA lane).
4. **Release fences are enqueued where the last reference is dropped.**
   `HIP_EVENT`: one event per frame, recorded on the library stream there,
   pending in the release-event table until then (the table of ADR-2023,
   moved to `core/src/vmafx/release_events.c` and shared by both lanes; on
   HIP it is also what keeps `vmafx_fence_wait()` from reporting an
   unrecorded event as signalled). `HOST`: a host function on the library
   stream. The release callback of `VmafxFrameImport` runs after both were
   enqueued. A `SYNC_FILE` release fence is refused with `VMAFX_E_NOTSUP`
   naming `release_fence`'s kind: there is no way to make one from a stream
   point on this runtime.
5. **dma-bufs are external memory, imported with their own size.** A
   `VMAFX_MEMORY_DMABUF` plane's descriptor is duplicated
   (`F_DUPFD_CLOEXEC`), imported with `hipImportExternalMemory()` as an
   opaque file descriptor of the size the dma-buf reports (`lseek(SEEK_END)`)
   and mapped whole; the plane is the mapping plus its offset. Planes on one
   descriptor share one import. A plane past the dma-buf's end is refused
   with `VMAFX_E_RANGE` naming `plane[i].size`; a modifier other than linear
   or a plane index other than 0 is `VMAFX_E_NOTSUP`. The mapping and the
   import are released behind an event on the library stream and destroyed
   when it has completed (checked at the next import and at close), never
   with a device-wide synchronisation on the import path.
6. **Producer memory is bound or converted on the device.** Planar device
   pointers and dma-buf planes are read in place, at any offset and pitch
   (the copies of rule 2 take any pitch). NV12 / P010 / P016 are planarised
   by the conversion kernels ADR-2023 introduced, now in
   `core/src/vmafx/import_convert_kernels.h` and compiled for both backends,
   into memory from `hipMallocAsync()` on the library stream that
   `hipFreeAsync()` returns at release. HIP arrays and GL textures are read
   out with `hipMemcpy2DFromArrayAsync()`; a planar frame in arrays is a
   device copy and needs `VMAFX_IMPORT_ALLOW_COPY`, as on CUDA.
7. **The import's work is submitted at import.** Right after enqueuing an
   import's work the library calls `hipStreamQuery()` on the library stream,
   which submits it. On the gfx1036 this took the array clip from 5 wrong
   runs in 6 to 0 in 8.
8. **HIP-GL only from a GLX context of the device's GPU.** Before any call
   into the HIP-GL interop, the import checks that a GLX context is current
   and that its renderer is the device's GPU (PCI bus id from the GLX
   interop query); otherwise the import is refused with `VMAFX_E_NOTSUP`
   naming `desc.memory`, and the runtime's interop is never touched. GL
   entry points are resolved at run time; textures are registered read-only,
   mapped on the library stream and unmapped there at release.
9. **`float_vif_hip` is registered by default.** The build option
   `enable_float_vif_hip_autodispatch` gated `float_vif_hip` behind a flag
   until the picture pool reached the twins (ADR-0623, T7-10c); device frames
   are that condition, and a twin without the flag cannot be picked for them.
   The option stays and now defaults to `true`.
10. **The platform's dropped commands are reported, not hidden.** The device
    tests compare every value and repeat a cell or a fence arm that differs,
    up to a bound, and print every repeat; a cell that differs in every
    attempt fails. A planted defect (skipped acquire wait, early release)
    differs in every attempt and is seen.
11. **No frame pools on HIP yet.** `vmafx_frame_pool_create()` refuses a HIP
    device with `VMAFX_E_NOTSUP` naming `device`: a pool frame carries no
    acquire fence, and the ADR-1199 barrier that orders a CUDA pool frame's
    write has no HIP counterpart (`T-HIP-VMAFX-NO-FRAME-POOLS-2026-10-06`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Twins copy device pictures on the library stream; the twin's stream and the null stream wait (chosen) | Every read of producer memory is on one stream, so the release is ordered after it; the twins' kernels are unchanged | A device-to-device copy per frame into the twins' buffers (the shared frame dedups it across twins), where the CUDA twins read the picture | Chosen; reading the producer's memory in each kernel is an RC8 tuning row (`T-HIP-IMPORT-TWIN-DEVICE-COPY-2026-10-06`) |
| Teach every HIP twin to read a device picture in place | No copy | Every twin's buffers, pitches and streams change in the lane that should only add the import path; the exact-twin contracts of RC3 would be re-proven for all of them | Not chosen now |
| Copy on the twin's own stream after a wait on the acquire | No library stream | Readers on as many streams as twins, so a release enqueued anywhere is not after all of them | Not chosen |
| sync_file as an external semaphore (wait and signal on the device, through a DRM syncobj) | No host check; a release fence of the same kind | `hipImportExternalSemaphore()` of the syncobj aborts the process on this runtime, a timeline descriptor is refused (measured) | Rejected after measurement |
| sync_file acquire checked with `poll()` on the host, D8 waits (chosen) | Correct for any producer; the same as the GL sync | A host wait when the producer is behind | Chosen |
| Fake a `SYNC_FILE` release fence (a host fence wrapped in a pipe) | Producers asking for one get one | A descriptor the kernel's sync_file machinery does not know; misleading | Not chosen: refused, named |
| dma-buf size from the producer | No system call | The runtime does not check it: a size past the buffer maps memory that is not the dma-buf's | Not chosen: the dma-buf's own size, the producer's checked against it |
| Destroy the external memory at release (`hipFree()`) | Simple | Synchronises the device, 200 ms behind a busy stream (measured), on the producer's thread | Not chosen: deferred behind an event |
| Leave the import's work for the next synchronisation to submit | No extra call | Later copies on another stream overtook it (5 of 6 runs wrong, measured) | Not chosen |
| HIP-GL without the GLX check | Any GL context | A failed first setup crashes later calls; another vendor's GLX context crashes the first | Not chosen |
| Keep `float_vif_hip` behind the option's old default | No default change | `float_vif` cannot be scored on HIP device frames in a default build | Not chosen |

## Consequences

- **Positive**: imported HIP frames score bit for bit as host-uploaded
  frames for every HIP twin declared exact (`scripts/ci/exact_twins.d/*.hip`)
  on the Netflix pair, both checkerboards, Sparks 10-bit and 4K, planar with
  odd offsets and pitches, semi-planar from pointers and from dma-bufs
  (`core/test/test_vmafx_import_hip_bitexact.c`); a skipped acquire wait, a
  skipped sync_file check and an early release are seen under device load
  (`test_vmafx_import_hip_fence.c`); GL textures import from a GLX context
  (`test_vmafx_import_hip_gl.c`); no plane goes through the host (runtime
  trace and the host-copy counter); one import is scored by two contexts.
  `psnr_hvs_hip`, `ssimulacra2_hip` and `float_ms_ssim_hip` no longer read
  device frames on the host.
- **Negative**: the twins copy each device frame once more on the device
  (into their buffers); frames of one device are copied on one stream; a
  sync_file acquire is a host wait when the producer is behind and a
  sync_file release fence is not available; a dma-buf and a GL texture are
  imported and registered per frame (caches are tuning rows); HIP-GL works
  only from GLX; a HIP device has no frame pools; `VMAFX_DEVICE_PROFILING`
  stays `VMAFX_E_NOTSUP` on HIP.
  On the gfx1036 the platform's dropped commands still give a wrong value
  about once per 10^4 frames, as before the import path.
- **Neutral / follow-ups**: the FFmpeg filter (WP9) can import VAAPI / DRM
  frames through the dma-buf path; a ROCm update that makes external
  semaphores work reopens the sync_file release (state row
  `T-HIP-ROCM-EXTERNAL-SEMAPHORE-ABORT-2026-10-06`).

## References

- [ADR-1829](1829-rc4-zero-copy-import.md), [ADR-1852](1852-vmafx-api-redesign.md) design section 2.7, [ADR-1929](1929-vmafx-device-frames-fences.md), [ADR-2023](2023-vmafx-cuda-device-frames.md), [ADR-1897](1897-vmafx-abi-0x-numbering.md), [ADR-1408](1408-hip-shared-frame-planes.md), [ADR-0623](0623-scaffold-audit-p2-half-finished.md), [Research-2159](../research/2159-vmafx-hip-device-frames.md).
- `req` (RC4 work package 3, HIP lane): "device pointer; dma-buf as external memory (new path) | HIP event; `sync_file` | HIP host device (dropped-dispatch defect: re-run before blaming)".
- `req` (RC4 work package index, added 2026-10-06, #2238): "OpenGL interop import (CUDA-GL, SYCL via EGL DMA-buf export, HIP-GL) and a GL sync fence kind (additive, ADR-1897)".
- Tests: `core/test/test_vmafx_import_hip.c`, `test_vmafx_import_hip_bitexact.c`, `test_vmafx_import_hip_fence.c`, `test_vmafx_import_hip_gl.c`, `test_vmafx_import_hip_contract.py`, `test_vmafx_fence_kinds.c`, `test_hip_shared_frame.c`.
