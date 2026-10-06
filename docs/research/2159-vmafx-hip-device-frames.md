<!-- markdownlint-disable MD013 MD060 -->
# Research-2159: What the HIP runtime of a gfx1036 does with imported frames, fences and GL textures

- **Status**: Active
- **Workstream**: [ADR-2092](../adr/2092-vmafx-hip-device-frames.md), [ADR-1829](../adr/1829-rc4-zero-copy-import.md), [ADR-2023](../adr/2023-vmafx-cuda-device-frames.md)
- **Last updated**: 2026-10-06

## Question

The HIP lane of RC4 work package 3 imports device pointers, dma-bufs, arrays
and GL textures with HIP event, sync_file and GL sync fences. Which runtime
calls behave as the CUDA lane's counterparts do, and which need another
design, on the project's pinned ROCm?

## Sources

- The pinned toolchain: ROCm 10.1.0, HIP runtime 7.16.26385
  (`hipRuntimeGetVersion()` 71626385), the image `build-config.env` names as
  `ROCM_BUILDER`:
  `rocm/dev-ubuntu-26.04:10.1.0-full@sha256:4f5ed1bf6532a4b9920400401b0ad0f706af356dae61e40afefbfd6c8073f0ce`,
  with meson, nasm, zimg, gbm, drm, X11 and Mesa 26.0.8 GLX added for the
  build and the tests. Run with `/dev/kfd` and `/dev/dri` passed through, the
  device's groups added, the X display mounted, under the device lock.
- The device: the AMD gfx1036 iGPU of a Ryzen 9 9950X3D, Linux
  7.2.9-1-cachyos, 2026-10-06. Other sessions shared the host (load average
  10 to 70).
- For comparison: the host's own ROCm 7.2.4 (HIP 7.2.53211, 70253211) with
  Mesa 26.2.4, on which the lane was first built the same day; and the 10.1
  runtime libraries run on the host with Mesa 26.2.4, to separate the
  runtime from Mesa.
- Probes: small C and HIP programs against the runtime; the lane's device
  tests (`core/test/test_vmafx_import_hip*.c`); `rocprofv3` and the runtime's
  API log (`AMD_LOG_LEVEL=3`) of the import sessions.
- `scripts/dev/hip_dispatch_drop_probe.hip` for the platform defect.

## Findings

| What | ROCm 10.1.0 (pinned) | ROCm 7.2.4 (host) | Consequence |
| --- | --- | --- | --- |
| External semaphores (no sync_file handle type exists) | A DRM syncobj from `drmSyncobjHandleToFD()` as an opaque descriptor: `hipErrorNotSupported`; as a timeline descriptor: `hipErrorInvalidValue` | The opaque descriptor aborts the process (`rocdevice.hpp:281: NullDevice::importExtSemaphore ... ShouldNotReachHere()`); timeline: `hipErrorInvalidValue` | sync_file is an acquire fence checked on the host; no `SYNC_FILE` release fence |
| `hipImportExternalMemory()` of a dma-buf | Imports with any `size` it is given (4 times the buffer included) and leaves the descriptor open | Same | Import the dma-buf's own size (`lseek(SEEK_END)`), refuse a larger producer size, import a duplicate descriptor |
| `hipFree()` of a mapping | 200.1 ms while another stream ran a 200 ms host function: it synchronises the device | Same (200 ms) | Release external memory behind an event, reap it later |
| `hipEventQuery()` on an event never recorded | `hipSuccess` | Same | Release events stay in the pending table until recorded |
| `hipStreamWaitEvent()` then `hipEventDestroy()` | The wait still holds | Same | Transient events for the copy waits are safe |
| Memory pools | Supported; `hipMallocAsync()` / `hipFreeAsync()` work | Same | Converted planes come from the stream's pool |
| A dma-buf written through GBM | With the check skipped, 32 imports took an unsignalled sync_file and still scored right: the import waits for the pending write | Same; the exported sync_file stays unsignalled about 1.5 ms after the unmap, a mapping made before it reads stale data | Producers pass the exported sync_file as the acquire fence; the implicit wait is not relied on |
| Unsubmitted stream work | Without `hipStreamQuery()` after an import, a planar array clip's last frame is wrong (6 values) in 8 of 8 runs, every attempt; with it, 0 | 5 of 6 runs wrong; with it, 0 of 8 | Each import submits its work |
| Null-stream readers | `integer_adm_hip`, `psnr_hip` and `float_vif_hip` launch on the null stream | Without a null-stream wait on the copies, frame 0 of `adm` had 7 wrong values | The copies' event is waited on by the reader's stream and by the null stream |
| HIP-GL setup | `hipGLGetDevices()` with no context: `hipErrorInvalidValue`, and a later call from a GLX context works; under another vendor's GLX context: `hipErrorInvalidValue` | After a failed first setup every later HIP-GL call crashes the process; another vendor's GLX context crashes `hipGLGetDevices()` | A GLX context on the device's GPU is checked before any HIP-GL call |
| HIP-GL read-out | A 640x360 R8 texture registers and maps (`hipArrayGetInfo()`: 8-bit, 640x360); `hipMemcpy2DFromArray(Async)()`, `hipMemcpyParam2DAsync()` and `hipMemcpy3DAsync()` return `hipErrorInvalidValue`; a row-wise `hipMemcpyFromArray()` and a texture object read by a kernel fault the GPU (page not present). The same with the image's Mesa 26.0.8 and with the host's Mesa 26.2.4, registered read-only or not, `glTexStorage2D()` or `glTexImage2D()` | Reads it: 0 of 230400 samples wrong | GL imports are refused on 10.1 with `VMAFX_E_NOTSUP` naming `desc.memory` and the runtime (`T-HIP-ROCM10-GL-TEXTURE-READ-2026-10-06`) |
| Dropped commands (`T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01`) | The probe, 5 runs of 100000 frames: 134, 98, 179, 110 and 103 bad frames | 3 runs of 20000 frames: 0, 17 and 3 bad frames | Tests repeat a differing cell up to four times and print each repeat |

## Exit evidence of the lane (ROCm 10.1.0)

| Check | Result |
| --- | --- |
| Imported equals host-uploaded for every HIP twin declared exact (`test_vmafx_import_hip_bitexact`) | Netflix 576x324 (69 cells, 11808 values), checkerboard 1 px and 10 px (72 cells, 765 values each), Sparks 10-bit (69 cells, 1230 values), BBB 3840x2160 (72 cells, 1530 values): 0 cells differing, no attempt repeated; planar with odd offsets and pitches, semi-planar from pointers and from dma-bufs; 9042 imports, 6028 conversions |
| Host copies | Test counter 0. `rocprofv3 --memory-copy-trace --hip-runtime-trace --kernel-trace` of the checkerboard import sessions (432 imports): the library stream ran 441 copy kernels (`__amd_rocclr_copyBufferRect*`), 288 NV12 conversions and 90 `float_ms_ssim_hip` level-0 conversions and no memory copy; the host-to-device copies are the test producer's uploads (12) and the twins' tables (6), the device-to-host copies the twins' result read-backs on their own streams. The Netflix sessions with the runtime trace abort `rocprofv3` at finalisation (`ring_buffer.cpp:106 mmap failed with errno 22`); their API log shows 7056 device-to-device copies on the library stream and no other copy there |
| Acquire wait under load (`test_vmafx_import_hip_fence`) | HIP event: `psnr` and `vif` 0 bad of 16 with the wait, 16 of 16 with it skipped; sync_file: 32 pending at import, 32 waited for, 0 bad |
| Release canary | 0 bad of 16; with the release planted early, 16 of 16 |
| One import, two contexts | 56 values each, 0 differing |
| Arrays, dma-bufs | NV12, P010 (16-bit) and planar arrays, planar and NV12 dma-bufs: 56 values each, 0 differing |
| GL textures (`test_vmafx_import_hip_gl`) | Skipped with the runtime's refusal (see above); on 7.2.4, 42 values, 0 differing |

## Conclusions

The HIP lane keeps the CUDA lane's shape (one library stream per device,
release fences recorded where the last reference goes, a pending table for
release events) and departs from it where the runtime differs: sync_file is
checked on the host, dma-buf memory is released behind an event, an import
submits its work, and HIP-GL is guarded by a GLX check and refused where the
runtime cannot read the mapped texture, which on the pinned ROCm 10.1 is
always. The twins copy imported frames on the device, where the CUDA twins
read them in place.
