<!-- markdownlint-disable MD060 -->
# HIP picture uploads and device state

The HIP extractors read host pictures, and the backend uploads them to the
device once per frame. This page covers how frames are uploaded and shared
between extractors, why there is no zero-copy import, and the order in which
an extractor uploads, clears its accumulators and launches. These notes
matter most to a program that creates several `VmafContext` objects in one
process; the `vmaf` tool creates one.

## Zero-copy import

The VMAFx API imports frames a producer holds on a HIP device: device
pointers, dma-bufs (as external memory, `hipImportExternalMemory()`), HIP
arrays and OpenGL textures, with HIP event, sync_file and GL sync acquire
fences ([HIP devices](../../api/vmafx/index.md#hip-devices),
[ADR-2092](../../adr/2092-vmafx-hip-device-frames.md)). An imported frame is
a `VMAF_PICTURE_BUFFER_TYPE_HIP_DEVICE` picture that carries the device's
library stream. Where a twin uploads a host picture, it copies a device
picture on that stream instead, device to device, and its own stream and the
null stream wait for the copy (`vmaf_hip_picture_upload()`,
`vmaf_hip_stream_wait_library()` in `core/src/hip/picture_hip.c`); the shared
planes below are filled the same way. No plane of an imported frame is copied
to or from the host.

Frames given to `libvmaf.h` (`vmaf_read_pictures()`) still arrive with
`VMAF_PICTURE_BUFFER_TYPE_HOST` in system memory, and the planes the
extractors of a run read are copied to the device once per frame; see
[Picture uploads](#picture-uploads).

## Picture uploads

### Shared planes

A frame's planes are uploaded once, whatever the number of extractors
([ADR-1408](../../adr/1408-hip-shared-frame-planes.md)). The first HIP
extractor of a frame that needs a plane uploads it into a buffer the
`VmafContext` owns, together with the other planes the extractors read in the
frame before; every other extractor reads that device copy. A plane no
extractor needs is not uploaded, so a run whose features read luma only (such
as `vmaf_v0.6.1`) uploads no chroma. The default model `vmaf_v1.0.16_3d0h` is
not luma-only: `speed_chroma_hip` reads U and V for `speed_chroma_uv` and
uploads them itself, outside the shared planes.

Thirteen extractors read the shared planes: `psnr_hip`,
`float_psnr_hip`, `float_moment_hip`, `ciede_hip`, `integer_ssim_hip`,
`float_ssim_hip`, `vif_hip`, `float_vif_hip`, `adm_hip`, `float_adm_hip`,
`motion_hip`, `motion_v2_hip` and `float_motion_hip`. With all of them in one
process a 4:2:0 frame pair used to be uploaded as 31 planes; it is now 6.

### Waiting upload

The upload does not return until the copy has finished reading the picture
(`vmaf_hip_picture_upload()`, `core/src/hip/picture_hip.h`). The pictures are
pageable host memory that the caller refills as soon as
`vmaf_read_pictures()` returns, and `hipMemcpy2DAsync` alone can still be
reading at that point. Pictures are read only while their frame is being
submitted, never afterwards.

### What this means for a run

- Scores are reproducible: every extractor gives the same per-frame output on
  every run and agrees with the CPU within its parity tolerance, and sharing
  the planes changes no output bit (thirteen extractors, both shipped models,
  576x324 to 3840x2160, 8 and 10 bits, with and without `--subsample`).
  Longer runs on a gfx1036 also show rare wrong frames that have nothing to
  do with uploads; see
  [The gfx1036 loses stream commands](overview.md#the-gfx1036-loses-stream-commands).
- The host waits for the device when planes are uploaded, which is once per
  frame from the second frame on, instead of once per extractor.
- Throughput on a gfx1036, ms per frame before and after sharing:
  `--model version=vmaf_float_v0.6.1` 57.3 to 46.9 at 1920x1080 (17.5 to
  21.3 frames per second) and 294 to 226 at 3840x2160, which gives back what
  the per-extractor wait had cost that model; `--model version=vmaf_v0.6.1`
  37.4 and 37.4 at 1080p; thirteen extractors in one process 183 and 184 at
  1080p, because their kernels are nearly all of the time; `motion_hip` and
  `motion_v2_hip` on their own 12.4 to 11.0 at 4K.
- `--subsample` is safe: an extractor that skips frames keeps the planes of
  the last frame it read until its kernels have finished, and the next upload
  into those buffers waits for the device first.

### Twins that stage their own copy

Extractors that convert or pack their input on the host still stage it
themselves: `integer_ms_ssim_hip` (to `float`), `psnr_hvs_hip`, `cambi_hip`,
`speed_chroma_hip`, `speed_temporal_hip` and `ssimulacra2_hip`. They copy the
picture into pinned memory they own before `submit()` returns
(`vmaf_hip_picture_upload_staged()` for CAMBI and SpEED,
[ADR-1378](../../adr/1378-hip-cambi-device-resident.md),
[ADR-1384](../../adr/1384-hip-speed-device-resident.md)) and the device copy
runs from that buffer without a wait.
`T-HIP-SHARED-FRAME-REMAINING-TWINS-2026-10-01` in
[`docs/state.md`](../../state.md) tracks moving them onto the shared planes.

### Measured strategies

Three upload strategies were measured on the gfx1036 for the shared planes
([Research-1408](../../research/1408-hip-shared-frame-planes.md)): the waiting
upload, a host copy into pinned memory that the kernels read in place, and a
host copy into pinned staging followed by a device copy. The waiting upload
was the fastest or tied in every configuration, because this iGPU's runtime
copies a pageable picture without a host copy. A discrete AMD GPU is
unmeasured.

### Checking a build

To check a build on your own hardware, run one extractor twice and compare:

```bash
for i in 1 2; do
  vmaf --reference ref.yuv --distorted dist.yuv \
       --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
       --backend hip --feature float_psnr_hip --no_prediction \
       --json --precision max --output run$i.json
done
cmp run1.json run2.json
```

Identical files do not prove the scores are right: with several extractors in
one process the old defect was deterministic. Compare against
`--backend cpu --feature float_psnr` as well, or run:

```bash
python3 "$(git rev-parse --show-toplevel)/scripts/ci/run_meson_test.py" -- \
  -C build test_hip_upload_race
```

That test also runs every extractor in one context, where the planes are
shared, with and without `n_subsample`, and refills both pictures the moment
a frame has been submitted. `test_hip_shared_frame` and
`test_hip_shared_frame_contract` check the sharing rules without a device.

## Clearing accumulators after the upload

Who this concerns: a program that creates more than one `VmafContext` with
HIP extractors in one process, for example to score a small clip and then a
larger one. The `vmaf` tool creates one context per process and was not
affected.

Before 2026-10-01 `float_moment_hip`, `vif_hip` and `adm_hip` returned a
wrong first frame in the first context of a process that needed larger
planes than the contexts before it. They cleared their accumulators ahead of
the frame's upload, and on a gfx1036 such a clear has no effect in that
situation: the frame's sums were added onto the sums the earlier context had
left in recycled device memory
([ADR-1427](../../adr/1427-hip-clear-after-upload.md)). Every HIP extractor
now uploads, then clears, then launches its kernels.

One frame in a 640x360 context, then one in a 3840x2160 context of the same
process, on `ryzen-4090-arc` (gfx1036, ROCm 7.2.4):

| First frame of the 3840x2160 context | Before | After | CPU |
|---|---|---|---|
| `float_moment_hip`, `float_moment_ref1st` | 130.53 | 127.00 | 127.00 |
| `vif_hip`, scale 0 | 0.6748 | 0.6934 | 0.6934 |
| `vif_hip`, scale 2 | 0.8749 | 0.8988 | 0.8988 |
| `adm_hip` | the run fails | identical | - |

The other eleven extractors of the test were correct before and are now.
Scores of later frames, of the first context of a process and of the `vmaf`
tool do not change. Re-run stored `float_moment_hip`, `vif_hip` or `adm_hip`
scores only if they came from a process that scored clips of rising size
through the library.

Time per frame is unchanged within the spread of the samples (medians of
three interleaved runs, `vif_hip` at 1080p of ten, other lanes loading the
host):

| Extractor | 1920x1080 before | after | 3840x2160 before | after |
|---|---|---|---|---|
| `float_moment_hip` | 1.95 | 1.97 | 7.22 | 7.14 |
| `vif_hip` | 46.86 | 47.84 | 207.30 | 202.10 |
| `float_psnr_hip` | 0.92 | 0.93 | 3.71 | 3.85 |

To check a device:

```bash
python3 "$(git rev-parse --show-toplevel)/scripts/ci/run_meson_test.py" -- \
  -C build test_hip_first_frame_clear_vif_hip \
  test_hip_first_frame_clear_float_moment_hip test_hip_first_frame_clear_adm_hip
```

There is one such test per extractor, each in its own process, because only
the first larger context of a process is exposed.
`test_hip_clear_after_upload_contract` checks the order in every HIP source
without a device.
