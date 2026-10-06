<!-- markdownlint-disable MD013 MD060 -->
# ADR-2023: VMAFx device frames on CUDA: one library stream per device, fences on it, release events recorded where the frame is released

- **Status**: Accepted
- **Date**: 2026-10-06
- **Deciders**: maintainer (popup 2026-10-06); RC4 work package 3 (CUDA lane)
- **Tags**: api, abi, rc4, gpu, cuda, opengl

## Context

[ADR-1929](1929-vmafx-device-frames-fences.md) fixed the shared contract of
zero-copy frame import (devices, fences, imports, release fences, pools,
admission, the D8 import rule) and implemented it on the CPU device. The CUDA
lane of RC4 work package 3 implements it on CUDA devices
([ADR-1829](1829-rc4-zero-copy-import.md) exit evidence): a device pointer or
array imported in the device's context without a device-to-device copy into
the pool, a CUDA event acquire fence waited on by the library's stream, a
release event recorded after the last reader, and the per-frame context
barrier of [ADR-1199](1199-cuda-picture-handover-barrier.md) kept only for
callers without fences. The work package's 2026-10-06 addition (#2238, an
interop path for screen-capture pipelines) adds an OpenGL texture import with
a GL sync acquire fence, as additive kinds
([ADR-1897](1897-vmafx-abi-0x-numbering.md)).

The engine's CUDA twins read device pictures that carry a stream, a ready
event and a finished event, and order their reads against the producer only
through the picture's stream and ready event; for pictures a caller wrote
itself, ADR-1199 synchronises the whole context once per frame. Implementing
the lane left choices the existing design does not make: which stream reads
an imported frame, how a release event can be handed out before it can be
recorded, what the extractors need of a producer's memory layout, and how a
GL sync becomes an ordering on the device. Two of them were measured before
they were decided (see Alternatives).

## Decision

We implement VMAFx device frames on CUDA with these rules.

1. **One library stream per device.** A CUDA device opened by index retains
   that device's primary context and creates a non-blocking stream; one made
   from the caller's objects uses its context (`external[0]`) and, when given,
   its stream (`external[1]`). Every frame of the device (imports and pool
   frames) is a CUDA device picture whose stream is that library stream. The
   CUDA twins read picture planes on the reference picture's stream, and
   `integer_adm` / `integer_cambi` read the distorted picture on its own stream
   (an audit of every twin), so every read of a frame of the device is on the
   library stream, and work enqueued there after the last read runs after it.
2. **Acquire fences are device-side.** A `CUDA_EVENT` acquire fence becomes a
   `cuStreamWaitEvent()` on the library stream; the frame's ready event is
   recorded behind it and behind its conversions, and the picture is marked
   ordered. The ADR-1199 context barrier is skipped for a frame pair whose
   pictures are both ordered and kept for every other picture (pool frames,
   host uploads, `libvmaf.h` callers). A `HOST` acquire fence is passed when
   signalled and is `VMAFX_E_BUSY` otherwise, as on the CPU device (the D8
   helper waits and retries once).
3. **Release fences are enqueued where the last reference is dropped.** `HOST`:
   a host function on the library stream signals the host fence behind the
   last reader. `CUDA_EVENT`: one event per frame, recorded on the library
   stream there. A CUDA event cannot be recorded ahead of time and a stream
   wait on an event never recorded returns at once, so the library keeps each
   release event it hands out in a table until it is recorded:
   `vmafx_fence_wait()` answers "pending" for it, and a producer that waits on
   the device does so from the frame's release callback, which runs after the
   recording.
4. **`VmafxFrameImport` gains a release callback.** `release` and `user`
   (appended, ABI 0.1.4) are called once on the thread that drops the frame's
   last reference, after its release fences were enqueued: a producer makes
   its stream wait on the `CUDA_EVENT` release fence there and reuses its
   buffer without a host wait (the FFmpeg filter's pattern, design section
   5.3). A 0.1.2-sized descriptor stays valid.
5. **Producer memory is bound, not copied.** Planar device-pointer planes are
   the producer's memory. NV12 / P010 / P016 are planarised on the device by
   `core/src/cuda/import_convert.cu` (a de-interleave, and for P010 the shift
   by 6; outputs are inputs, so bit-exact with the host upload). A bound plane
   must be readable as the twins read their own pictures: rows 8-byte aligned,
   a pitch that is a multiple of 8 and at least the row rounded up to 8 bytes
   (`integer_vif` loads 4 or 8 bytes at a time). Any other plane is refused
   with `VMAFX_E_NOTSUP` naming its offset or pitch, or copied on the device
   with `VMAFX_IMPORT_ALLOW_COPY`.
6. **CUDA arrays and GL textures.** A semi-planar frame in CUDA arrays (or GL
   textures) is read out of the arrays and planarised on the device, a
   conversion like the device-pointer one; a planar frame in arrays is a
   device copy and needs `VMAFX_IMPORT_ALLOW_COPY`.
   `VMAFX_MEMORY_GL_TEXTURE` (8) and `VMAFX_FENCE_GL_SYNC` (8) are added: the
   textures of the GL context current on the importing thread are registered
   read-only and mapped on the library stream (the map orders the GL commands
   before it), unmapped there behind the last reader at release (the unmap
   orders later GL commands after the reads). A GL sync is checked with
   `glClientWaitSync()`; unsignalled, the import is `VMAFX_E_BUSY` and the D8
   helper waits on it on the host. GL entry points are resolved at run time.
7. **Contexts score on the device.** `vmafx_context_use_device()` creates the
   engine's CUDA state on the device's context and imports it; feature
   registration on a device context picks the CUDA twin, and registers the CPU
   extractor with a warning when the backend has none or the twin cannot
   honour an option, which admission then names for device frames. Every CUDA
   twin reads a CUDA device picture (the per-extractor admission answer).
8. **No host copy, counted.** A device frame of the VMAFx API is never
   downloaded: the engine refuses it where a CPU extractor would need a host
   copy, and the engine's device-to-host download counts toward
   `vmafx_count_host_copy()`. The one CUDA twin that read its inputs through
   the host, `float_ms_ssim_cuda` (a copy of every plane to pinned memory for
   `picture_copy()`, found by the vendor-profiler trace), converts on the
   device now with the same arithmetic. The driver library is loaded once
   per process, so fences are waited on and destroyed without a device.
9. **`integer_vif` reads each picture with its own pitch.** Found on the way:
   the twin read both the reference and the distorted picture with a pitch it
   derived at init from the device's texture alignment, which equals the
   pitch of the engine's own pictures only; an imported plane with another
   pitch was read row-shifted. It now takes each picture's `stride[0]`.
10. **A CUDA pool frame is handed out after its readers ran.** A pool frame
    returns to its pool where its last reference is dropped, which may be
    before the device ran its readers; its acquire waits for an event recorded
    behind them, so the caller's next write, on a stream the library does not
    know, cannot overwrite a frame still being read (measured: 5 of 16 frames
    wrong without the wait). A pool frame carries no acquire fence, so the
    ADR-1199 barrier orders the caller's write before the readers (15 of 16
    wrong without it).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| One library stream per device (chosen) | Every reader of a frame is on one stream, so a release enqueued there is after the last reader; two contexts scoring one import need nothing more | No overlap of two frames' kernels across streams (an RC8 tuning row: measured, and held to the same fence tests) | Chosen |
| A stream per imported frame | Overlap across frames | The twins read the distorted picture on the reference picture's stream: a frame's readers are on another frame's stream, which the release cannot know | Not chosen |
| Release event recorded at release, with a pending table and a release callback (chosen) | No stream waits on anything the device cannot see; a device wait from the callback is ordered; a host wait is correct at any time | A table of handed-out events (4096 slots); a device wait before the callback is the caller's error | Chosen |
| Release event recorded at request time behind a gate stream (`cuStreamWaitValue32` on a word the library stream writes at release) | A device wait is correct at any time | Measured: a stream blocked on a gate holds up every `cuCtxSynchronize()` in the context (the engine's flush, the ADR-1199 barrier), which deadlocked a second context still reading the frame; the write also needs an explicit flush of the library stream | Rejected after measurement |
| Skip the ADR-1199 barrier for fenced pairs only (chosen) | Fenced imports are ordered on the device without a host stall; compat callers keep their guarantee | Two code paths | Chosen |
| Keep the barrier for every frame | Simple | A host-side context synchronisation per frame, and it hides a missing acquire wait (the fence tests could not see one) | Not chosen |
| Refuse or copy-with-flag an unaligned bound plane (chosen) | No silent copy; producers' usual layouts (decoder, hardware frame pools) are aligned | A producer with an unusual layout must align or opt in | Chosen |
| Always copy an unaligned plane | Any layout works | A silent device copy against the zero-copy default | Not chosen |
| Make every twin read unaligned rows | Any layout bound | Slower loads in `integer_vif`'s vertical pass for a layout no producer uses | Not chosen (a tuning item if one does) |
| GL sync checked on the host, D8 waits (chosen) | Correct for a sync of any shared context | A host wait when the producer is behind | Chosen |
| `glWaitSync()` before the map | No host wait | Relies on the map ordering the current context's server wait as well as its commands, which the interop documentation does not state | Not chosen |
| Arrays read out by a copy, then the pointer kernels (chosen) | One set of conversion kernels for pointers and arrays | One device copy of the chroma per semi-planar array import | Chosen |
| Texture objects read by the conversion kernels | No intermediate copy | A second set of kernels and texture-object lifetimes across the stream | Not chosen |

## Consequences

- **Positive**: imported CUDA frames score bit for bit as host-uploaded frames
  for every CUDA twin declared exact (the Netflix pair, both checkerboards,
  Sparks 10-bit and 4K, planar and semi-planar,
  `core/test/test_vmafx_import_cuda_bitexact.c`); a skipped acquire wait and an
  early release are seen under device load (`test_vmafx_import_cuda_fence.c`);
  GL textures import with a GL sync (`test_vmafx_import_cuda_gl.c`); the
  `integer_vif` twin no longer assumes the pitch of its pictures, and
  `float_ms_ssim_cuda` no longer round-trips frames through the host.
- **Negative**: frames of one device are read on one stream, so two frames'
  kernels do not overlap; multi-stream overlap is an RC8 tuning row, measured
  and held to the same fence tests (`test_vmafx_import_cuda_fence.c`: acquire
  under load, release canary, pool frames without a fence); the release
  event table bounds frames with a pending `CUDA_EVENT` release fence at 4096;
  a GL texture is registered and unregistered per import (a registration cache
  is a tuning item); `VMAFX_DEVICE_PROFILING` stays `VMAFX_E_NOTSUP` on CUDA
  (the vendor profiler serves).
- **Neutral / follow-ups**: WP9's FFmpeg filter imports `AV_PIX_FMT_CUDA`
  frames with the device made from the frames context's context and stream
  and waits on the release event from the release callback; the SYCL / HIP
  lanes reuse the GL kinds and the release-callback field.

## References

- [ADR-1829](1829-rc4-zero-copy-import.md), [ADR-1852](1852-vmafx-api-redesign.md) design sections 2.7 and 5.3, [ADR-1929](1929-vmafx-device-frames-fences.md), [ADR-1199](1199-cuda-picture-handover-barrier.md), [ADR-1897](1897-vmafx-abi-0x-numbering.md), [ADR-1223](1223-cuda-ampere-architecture-floor.md), [ADR-1462](1462-cuda-vif-reads-host-log2-table.md).
- `Q` (maintainer popup 2026-10-06, ADR-2023): "Accept, overlap as tuning later (Recommended)": multi-stream overlap becomes an RC8 tuning row, measured, with the same fence tests.
- `req` (RC4 work package 3, CUDA lane): "device pointer / array in the device's context; no device-to-device copy into the pool | event wait on the library stream; release event recorded after last reader; ADR-1199 barrier stays only for compat callers without fences".
- `req` (RC4 work package index, added 2026-10-06, #2238): "OpenGL interop import (CUDA-GL, SYCL via EGL DMA-buf export, HIP-GL) and a GL sync fence kind (additive, ADR-1897)".
- Tests: `core/test/test_vmafx_import_cuda.c`, `test_vmafx_import_cuda_bitexact.c`, `test_vmafx_import_cuda_fence.c`, `test_vmafx_import_cuda_gl.c`, `test_vmafx_import_cuda_cells_contract.py`.
