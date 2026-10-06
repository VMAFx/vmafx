<!-- markdownlint-disable MD013 MD060 -->
# Research-2159: What the HIP runtime of a gfx1036 does with imported frames, fences and GL textures

- **Status**: Active
- **Workstream**: [ADR-2092](../adr/2092-vmafx-hip-device-frames.md), [ADR-1829](../adr/1829-rc4-zero-copy-import.md), [ADR-2023](../adr/2023-vmafx-cuda-device-frames.md)
- **Last updated**: 2026-10-06

## Question

The HIP lane of RC4 work package 3 imports device pointers, dma-bufs, arrays
and GL textures with HIP event, sync_file and GL sync fences. Which runtime
calls behave as the CUDA lane's counterparts do, and which need another
design?

## Sources

- AMD gfx1036 (the integrated GPU of a Ryzen 9 9950X3D), ROCm 7.2.4 (HIP
  runtime from `rocm-systems-rocm-7.2.4`), Linux 7.2.9-1-cachyos, Mesa GLX,
  2026-10-06. Other sessions shared the host (load average 20 to 70).
- Probes: small C and HIP programs against the runtime, each run under the
  device lock; the lane's device tests
  (`core/test/test_vmafx_import_hip*.c`); the runtime's API log
  (`AMD_LOG_LEVEL=3`) of one bit-exactness run.
- `scripts/dev/hip_dispatch_drop_probe.hip` for the platform defect.

## Findings

| What | Measured | Consequence |
| --- | --- | --- |
| External semaphores | No sync_file handle type exists. An opaque descriptor (a DRM syncobj from `drmSyncobjHandleToFD()`) aborts the process in `hipImportExternalSemaphore()`: `rocdevice.hpp:281: NullDevice::importExtSemaphore ... ShouldNotReachHere()`. A timeline semaphore descriptor returns `hipErrorInvalidValue` | sync_file is an acquire fence checked on the host; no `SYNC_FILE` release fence |
| `hipImportExternalMemory()` of a dma-buf | Imports with any `size` it is given, past the buffer's end included, and leaves the descriptor open (the caller's to close) | Import the dma-buf's own size (`lseek(SEEK_END)`), refuse a larger producer size, import a duplicate descriptor |
| `hipFree()` of a mapping | 200 ms while another stream ran a 200 ms host function: it synchronises the device | Release external memory behind an event, reap it later |
| `hipEventQuery()` on an event never recorded | `hipSuccess` | Release events stay in the pending table until recorded (as on CUDA, more so) |
| `hipStreamWaitEvent()` then `hipEventDestroy()` | The wait still holds | Transient events for the copy waits are safe |
| `hipStreamCreate()` | Waits for work on the device | The test-only early-release stream is created once |
| Memory pools | `hipDeviceAttributeMemoryPoolsSupported` 1; `hipMallocAsync()` / `hipFreeAsync()` work | Converted planes come from the stream's pool and return there at release |
| A dma-buf written through GBM, then mapped | The unmap lands the writes with an asynchronous copy: the dma-buf's exported sync_file is unsignalled for about 1.5 ms after the unmap; an import made after the write waits for that copy (about 2 ms), a mapping made before it reads stale data | Producers pass the exported sync_file as the acquire fence; the import's implicit wait is not relied on |
| Unsubmitted stream work | An array read-out enqueued on the library stream and left unsubmitted was overtaken by copies enqueued later on another stream that waited for it: the last frame of a planar array clip was wrong in 5 of 6 runs; with `hipStreamQuery()` after each import, 0 of 8 | Each import submits its work |
| Null-stream readers | `integer_adm_hip`, `psnr_hip` and `float_vif_hip` launch on the null stream; without a null-stream wait on the copies, frame 0 of `adm` had 7 wrong values | The copies' event is waited on by the reader's stream and by the null stream |
| HIP-GL | The interop reads only the current GLX context. With no context, an EGL context or another vendor's GLX context, its first setup fails and every later HIP-GL call crashes the process; `hipGLGetDevices()` crashes outright under another vendor's GLX context | A GLX context on the device's GPU (PCI bus id from `glXGLInteropQueryDeviceInfoMESA`) is checked before any HIP-GL call |
| Dropped commands | The probe (12 kernels per frame, 20000 frames): 0, 17 and 3 bad frames in three runs (load average 24 to 38); a smaller probe of the import pattern (a plane copy, a zeroing kernel, a summing kernel, an event, a read-back) lost commands in 6 to 46 of 20000 frames under load average 25 to 67 with and without the copy | Tests repeat a differing cell up to four times and print each repeat (`T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01`) |

## Exit evidence of the lane

| Check | Result |
| --- | --- |
| Imported equals host-uploaded for every HIP twin declared exact (`test_vmafx_import_hip_bitexact`) | Netflix 576x324 (69 cells, 11808 values), checkerboard 1 px and 10 px (72 cells, 765 values each), Sparks 10-bit (69 cells, 1230 values), BBB 3840x2160 (72 cells, 1530 values): 0 cells differing in every attempt; 5 attempts repeated on the Netflix pair (15 values), 0 elsewhere; planar with odd offsets and pitches, semi-planar from pointers and from dma-bufs |
| Host copies | 0 (counter, 7104 imports and 4800 conversions on the Netflix pair); runtime log of that run: 7056 device-to-device copies on the library stream, no host-to-device or device-to-host copy there; the other copies are the test producer's uploads (480), the twins' constant uploads (33) and result read-backs (5592) |
| Acquire wait under load (`test_vmafx_import_hip_fence`) | HIP event: `psnr` and `vif` 0 bad of 16 with the wait, 16 of 16 with it skipped; sync_file: 32 pending at import, 32 waited for, 0 bad; with the check skipped, 32 taken unsignalled |
| Release canary | 0 bad of 16; with the release planted early, 16 of 16 |
| One import, two contexts | 56 values each, 0 differing; the release fence signalled after the last reader of either |
| GL textures (`test_vmafx_import_hip_gl`) | 42 values, 0 differing; without a GLX context the import is refused naming `desc.memory` |

## Conclusions

The HIP lane keeps the CUDA lane's shape (one library stream per device,
release fences recorded where the last reference goes, a pending table for
release events) and departs from it where the runtime differs: sync_file is
checked on the host, dma-buf memory is released behind an event, an import
submits its work, and HIP-GL is guarded by a GLX check. The twins copy
imported frames on the device, where the CUDA twins read them in place.
