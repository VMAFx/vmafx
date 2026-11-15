---
paths:
  - core/src/cuda/picture_cuda.c
  - core/src/libvmaf.c
  - core/src/meson.build
  - ffmpeg-patches/src/vf_vmafx.c
invariant: CUDA governing ADRs, FFmpeg vmafx filter contract, kernelParams count parity, and full-plane D2H download mask.
---
<!-- markdownlint-disable MD013 -->
# Governing ADRs, kernelParams parity, and D2H download

## Governing ADRs

- [ADR-0022](../../../../docs/adr/0022-inference-runtime-onnx.md) — CUDA execution provider mapping.
- [ADR-0027](../../../../docs/adr/0027-non-conservative-image-pins.md) — CUDA 13.2 + experimental flags.
- [ADR-0131](../../../../docs/adr/0131-port-netflix-1382-cumemfree.md) —
- [ADR-0982](../../../../docs/adr/0982-gpu-runtime-bug-audit-round-26.md) —
  GPU runtime bug audit — round 26 (init/teardown leak sweep).
  `vmaf_cuda_picture_free` synchronous free.
- [ADR-0202](../../../../docs/adr/0202-float-adm-cuda-sycl.md) —
  `float_adm_cuda` requires `--fmad=false` on its fatbin to
  match GLSL `precise` qualifier in `float_adm.comp`. Since
  [ADR-1403](../../../../docs/adr/1403-cuda-strict-fp-every-kernel.md) every
  fatbin has it through `cuda_device_strict_fp_args`
  (`core/src/meson.build`); ADR-0202's "one fatbin only" scope is
  superseded. **On rebase**: keep shared list on fatbin command.
- [ADR-1320](../../../../docs/adr/1320-cuda-hip-kernel-header-dependency-tracking.md) —
  CUDA fatbin and HIP HSACO kernel header dependency tracking via explicit depend_files and compiler depfiles.

- **FFmpeg `vmafx` filter — CUDA frame consumer** (fork-local, ADR-2125, ADR-2023):
  [`ffmpeg-patches/0003-add-vmafx-filter.patch`](../../../../ffmpeg-patches/0003-add-vmafx-filter.patch)
  (source `ffmpeg-patches/src/vf_vmafx.c`, `cuda_open` / `cuda_import`) opens the device with
  `vmafx_device_create()` and hands CUDA frames over with `vmafx_context_import_frame()`; it
  calls no `include/libvmaf/libvmaf_cuda.h` function. Patch `0010` (libvmaf `cuda` selector)
  is retired. **On rename / signature change of those VMAFx entry points**: patch 0003 updates
  in same PR. Verify by cumulative `git am --3way` replay of `ffmpeg-patches/series.txt`
  against pristine FFmpeg at the tag in `build-config.env`.

## Build

```bash
meson setup build -Denable_cuda=true -Denable_sycl=false
ninja -C build
```

Requires `/opt/cuda` + `nvcc` on PATH.

## Per-plane kernels: the signature must match `kernelParams` by count (ADR-1215)

`cuLaunchKernel` does not validate that kernel consumes every entry
of `kernelParams` array — kernel declared with five parameters,
launched with six, ignores sixth entirely. That's how
`calculate_psnr_kernel_16bpc` shipped reading `data[0]` for every
plane while host faithfully passed `plane`. Host dispatch shared
between bit-depth variants -> diff two kernel signatures against
params array before trusting parity test. Also make sure fixture's
chroma isn't flat, or wrong plane reads same sentinel as right one.

## Device-to-host picture download takes every plane (T-CUDA-DEVICE-INPUT-CHROMA-NOT-DOWNLOADED-2026-10-01)

- `translate_picture_device()` in `core/src/libvmaf.c` calls
  `vmaf_cuda_picture_download_async(pic, host, mask)` with mask `0x7` (`0x1` for
  4:0:0), the mask the host-to-device upload uses. Mask `0x1` leaves the chroma of
  the host picture uninitialised; CPU `psnr` then reads `psnr_cb` = 60 (cap) with
  no error (Netflix/vmaf#1613).
- Guard: `test_cuda_device_input_host_extractors` (`gpumask = 1` keeps CUDA twins
  off, DEVICE pool, `psnr` y/cb/cr `==` CPU run, `n_threads` 0 and 4).
