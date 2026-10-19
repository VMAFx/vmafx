---
paths:
  - core/src/hip/picture_hip.c
  - core/src/hip/shared_frame.c
  - core/src/hip/shared_frame.h
invariant: HIP is host-pic so kernels must stage device copies via shared frame and tightly packed row stride.
---
# Host Picture Staging and Shared Frame Acquisition

## The HIP backend is host-pic — stage before you launch (ADR-1211)

`VmafPicture::data[]` points at HOST memory for HIP (ADR-0530). HIP
kernel that reads picture planes therefore needs device copy
extractor makes itself; handing it `pic->data[i]` faults GPU:

```text
Memory access fault by GPU node-1 on address 0x... Reason: Page not present
```

and takes whole process with it — no graceful error, no skip, so
single extractor doing this makes `--backend hip` look completely
dead.

Correct shape is in `core/src/feature/hip/integer_psnr_hip.c`: ask
for device planes in submit (`vmaf_hip_plane_source_acquire()`,
`fex->hip_frame`; ADR-1408, `shared_frame.h`), close plane source
in close. Context uploads each plane once per frame for all twins;
twin allocates no picture staging of its own. Two things to get right:

- Device plane is tightly packed, so stride you pass to kernel is
  plane **width** times bytes per sample, not `pic->stride[i]`.
- Do not port CUDA twin's call shape verbatim. CUDA extractors
  receive device picture from pool, so their helpers take device
  pointer caller never had to produce. That mismatch is precisely
  how `integer_adm_hip` ended up faulting.

Exception since ADR-2092: VMAFx import = device picture
(`VMAF_PICTURE_BUFFER_TYPE_HIP_DEVICE`, `priv->hip.str` = library stream).
`vmaf_hip_picture_upload()` / `_upload_staged()` and shared frame copy it
device to device on that stream, then make reader's stream and null stream
wait (`vmaf_hip_stream_wait_library()`). Twin reading planes on host checks
`vmaf_hip_picture_device_stream()` first, copies on device instead
(`psnr_hvs_hip`, `ssimulacra2_hip`, `float_ms_ssim_hip` level 0). Mixed
host / device pair -> `-EINVAL`. See
[vmafx-device-frames](vmafx-device-frames.md).

When debugging fault here, `AMD_SERIALIZE_KERNEL=3
HIP_LAUNCH_BLOCKING=1 AMD_LOG_LEVEL=3` names offending kernel.
Faulting address in host heap range is tell that host pointer
reached device.
