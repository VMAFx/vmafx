<!-- markdownlint-disable MD013 MD060 -->
# Research-1408: one upload of each frame plane for all HIP twins — output identity, throughput and the rejected upload variants on a gfx1036

- **Status**: Active
- **Workstream**: RC3 HIP lane, `T-HIP-TWIN-PRIVATE-PLANE-UPLOADS-2026-09-29` and `T-HIP-UPLOAD-WAIT-THROUGHPUT-2026-09-19`; decision in [ADR-1408](../adr/1408-hip-shared-frame-planes.md)
- **Last updated**: 2026-10-01

## Question

Every HIP twin copied the planes it reads to the device itself and waited for the copy. What does a run gain when the `VmafContext` uploads each plane once per frame and the twins read that copy, does any score move, and which way of getting the planes onto the device is fastest on the one AMD device at hand?

## Sources

- `core/src/hip/shared_frame.{c,h}`, `core/src/hip/picture_hip.{c,h}`, the thirteen adopted twins under `core/src/feature/hip/`.
- [ADR-1369](../adr/1369-sycl-shared-planes-light-twins.md), the SYCL precedent; [ADR-1377](../adr/1377-hip-motion-diff-first.md), the staged upload.
- `/opt/rocm/include/hip/hip_runtime_api.h` (ROCm 7.2.4): `hipHostMalloc` "allocates pinned host memory which is mapped into the address space of all GPUs in the system, the memory can be accessed directly by the GPU device".
- Host: `ryzen-4090-arc`, AMD gfx1036 iGPU, ROCm 7.2.4, Linux 7.2.8. Other jobs ran on the host during every measurement (load average 6 to 35).
- Builds: `meson setup build-hip core -Denable_hip=true -Denable_hipcc=true -Dhip_gfx_targets=gfx1036 -Denable_cuda=false -Denable_sycl=false --buildtype=release -Db_lto=false`, once at `origin/master` 7dc45265f ("before") and once with the change on top ("after").

## Findings

### Which twins a run uses

`--backend hip` selects a twin for a feature only when the twin carries the HIP flag. `adm_hip` and `float_vif_hip` do not (the latter behind `enable_float_vif_hip_autodispatch`, off by default, ADR-0623), so they run when named and the models compute `adm` and `float_vif` on the CPU:

| Run | On the device | On the CPU |
|---|---|---|
| `--model version=vmaf_v0.6.1` | `motion_hip`, `vif_hip` | `adm` |
| `--model version=vmaf_float_v0.6.1` | `float_adm_hip`, `float_motion_hip` | `float_vif` |
| thirteen twins | `psnr`, `float_psnr`, `float_moment_hip`, `ssim`, `float_ssim`, `ciede`, `vif`, `adm_hip`, `motion`, `motion_v2`, `float_adm`, `float_vif_hip`, `float_motion` | none |

### Uploads and waits per frame

Counted from the planes each twin asks for (4:2:0 input, default options) and confirmed by the shared frame's counter in `test_hip_upload_race` (48 plane uploads in 8 frames with twelve twins in one context).

| Run | Planes uploaded per frame, before | After | Host waits per frame, before | After |
|---|---|---|---|---|
| Thirteen twins in one process | 31 | 6 | 11 | 1 |
| `vmaf_v0.6.1` | 3 | 2 | 1 | 1 |
| `vmaf_float_v0.6.1` | 3 | 2 | 2 | 1 |
| `float_adm` + `float_vif_hip` + `float_motion` | 5 | 2 | 3 | 1 |

An upload takes along the planes the twins asked for in the frame before, so from the second frame on a frame is one upload call, whichever twin asks first. Without that, the default model would wait twice (`motion_hip` for the reference luma, `vif_hip` for the distorted luma) where it waited once before, because the motion twins used to stage without a wait.

### No output bit changes

`origin/master` 7dc45265f against the change, `--backend hip --precision max`, JSON compared metric by metric and frame by frame.

| Run | Fixture | Frames | Metric series | Identical |
|---|---|---|---|---|
| Thirteen twins | Netflix 576x324 | 48 | 40 | all |
| Thirteen twins | Netflix 576x324, 10 bits | 3 | 40 | all |
| Thirteen twins | 1080p checkerboard, 1 px | 3 | 40 | all |
| Thirteen twins | 1080p checkerboard, 10 px | 3 | 40 | all |
| Thirteen twins | BBB 3840x2160 | 50 | 40 | all |
| Thirteen twins, `--subsample 2` | Netflix, BBB 4K (24 frames read) | 12 / 12 | 40 each | all |
| `--model version=vmaf_v0.6.1` | Netflix, checkerboard 1 px, BBB 4K | 48 / 3 / 50 | 15 each | all |
| `--model version=vmaf_float_v0.6.1` | Netflix, checkerboard 1 px, BBB 4K | 48 / 3 / 50 | 15 each | all |
| `psnr` + `psnr_hvs` + `motion_v2` | Netflix, BBB 4K | 48 / 50 | 10 each | all |

390 series, none with a differing frame. The twins' distance from the CPU is therefore what it was. Thirteen twins against `--backend cpu`, 22 frames: `adm_hip`, `motion`, `motion_v2`, `psnr`, `float_psnr` and `float_moment_hip` equal the CPU on every frame of the Netflix pair and of BBB 4K; max abs diff of the others, Netflix / 4K: `vif` 5.4e-7 / 3.0e-7, `ssim` 2.3e-14 / 5.6e-13, `float_ssim` 1.2e-7 / 2.4e-7, `float_adm` 1.9e-7 / 1.3e-5, `float_vif_hip` 3.8e-5 / 7.0e-6, `float_motion` 2.8e-6 / 2.4e-5, `ciede` 1.1e-5 / 1.4e-6.

### Throughput

ms per frame in steady state: the CLI's own frames-per-second line on a pty gives the wall time at frame 11 and at the last frame of one process, so process start-up and first-frame initialisation are not in the number. Median of the stated number of runs, the two builds interleaved. 4K is BBB 3840x2160 (50 frames measured), 1080p its centre crop (80 frames measured).

| Run | 1080p before | 1080p after | Runs | 4K before | 4K after | Runs |
|---|---|---|---|---|---|---|
| `--model version=vmaf_float_v0.6.1` | 57.30 | 46.86 | 7 | 294.32 | 226.43 | 5 |
| `--model version=vmaf_v0.6.1` | 37.36 | 37.37 | 9 | two modes, see below | | 5 |
| thirteen twins | 182.71 | 184.20 | 7 | 731.17 | 734.96 | 5 |
| `adm_hip` + `vif` + `motion` | 52.87 | 52.51 | 7 | 227.75 | 213.55 | 5 |
| `vif` + `motion` | 41.83 | 38.03 | 7 | 224.35 | 222.51 | 3 |
| `float_adm` + `float_vif_hip` + `float_motion` | 43.38 | 43.25 | 3 | 186.08 | 186.28 | 3 |
| `psnr` + `psnr_hvs` + `motion_v2` | 8.00 | 7.97 | 3 | 31.74 | 32.27 | 7 |
| `psnr` + `float_psnr` + `float_moment_hip` | 4.95 | 4.41 | 3 | 17.16 | 15.11 | 3 |
| `psnr` + `motion_v2` | 4.21 | 4.14 | 7 | 18.49 | 17.96 | 3 |
| `motion` | 2.71 | 2.71 | 7 | 12.39 | 11.04 | 3 |
| `motion_v2` | 3.09 | 3.02 | 3 | 12.41 | 10.95 | 3 |
| `psnr` | 1.63 | 1.67 | 7 | 7.14 | 7.35 | 7 |

Reading it:

- **The float model gets its throughput back.** `T-HIP-UPLOAD-WAIT-THROUGHPUT-2026-09-19` measured `vmaf_float_v0.6.1` losing 21% at 1080p when the per-twin wait went in (18.8 to 14.8 frames per second). It now runs at 21.3 frames per second where the parent runs at 17.5: six of the seven "after" samples lie between 45.8 and 47.6 ms, every "before" sample between 54.8 and 65.5. At 4K the "after" run is the faster one in each of the five interleaved pairs. An earlier pair of builds (`origin/master` 01ed95c88) gave 59.24 to 46.67 and 263.22 to 195.13. The mechanism is the CPU extractor next to the twins: `float_motion_hip` no longer waits behind `float_adm_hip`'s kernels for an upload of its own, so the host reaches the CPU `float_vif` of the frame while those kernels still run.
- **Runs that are all on the device do not move.** The thirteen twins, `adm_hip` + `vif` + `motion` and the three float twins are bound by their kernels (`vif_hip` alone is about 40 ms of a 1080p frame and 150 to 220 ms of a 4K frame), and 25 fewer plane uploads do not show in 180 or 730 ms.
- **The default model is unchanged**: 37.36 and 37.37 ms at 1080p over nine interleaved pairs.
- **Two speeds on this device, on both builds.** `vmaf_v0.6.1` at 4K: 192.64 / 149.09 / 160.52 / 230.38 / 233.39 before, 152.27 / 154.56 / 228.51 / 232.34 / 233.78 after; at 1080p, in other sets of runs, about 37 or about 56 on both builds. The slow runs coincide with other jobs loading the host (the iGPU shares the memory bus with them), so a median over a mixed set says nothing about the change; the rows above are from sets in which both builds saw both conditions.
- **Light runs gain a little**: the motion twins on their own at 4K (12.4 to 11.0 ms) because they no longer copy the luma into pinned staging on the host, and combinations of light twins because the later ones upload nothing (`psnr` + `float_psnr` + `float_moment_hip` 17.16 to 15.11 at 4K). `psnr` alone and the row's `psnr` + `psnr_hvs` + `motion_v2` are inside their spread; `psnr_hvs_hip` is most of the latter and still stages its own planes.
- **The device-to-device copy that keeps the motion twins' previous frame is not visible**: `psnr_hvs` + `motion_v2` 25.23 / 25.57 and `psnr_hvs` + `motion` 26.75 / 26.32 at 4K (five runs, an earlier pair of builds).

### Three ways to get a shared plane onto the device

Measured before the final design, `origin/master` 42dd0f75f against the change, `(t(22) - t(2)) / 20` between two processes, median of three, 4K.

| Run | Parent | Waiting device upload (chosen) | Pinned host planes read in place |
|---|---|---|---|
| `motion_v2` | 12.45 / 12.48 | 10.86 | 12.40 |
| `motion` | 12.34 / 12.58 | 10.86 | 12.18 |
| `psnr` | 7.12 / 7.06 | 7.21 | 9.70 |
| `psnr` + `motion_v2` | 18.53 / 19.15 | 17.61 | 22.00 |
| `vmaf_v0.6.1` | 231.0 / 230.2 | 232.4 | 230.8 |

"Pinned host planes" allocates each shared plane with `hipHostMalloc`, fills it with a host `memcpy` of the picture and hands the kernels that pointer: no device copy and no wait on the device queue. It gives the same scores, bit for bit, and is slower wherever the upload is a visible share of the frame: the host copy of a pageable 4K picture (25 MB for six planes) costs more than the runtime's own upload, which on this iGPU maps the pageable pages instead of copying them on the host. The third way, a host copy into pinned staging followed by a device copy without a wait (`vmaf_hip_picture_upload_staged()`), pays the same host copy and a device copy on top; `T-HIP-UPLOAD-WAIT-THROUGHPUT-2026-09-19` measured it at 13.24 ms for a single `motion_v2_hip` against 10.70 with the waiting upload.

### The wait still closes the race

With the shared upload replaced by a bare `hipMemcpy2DAsync` (no wait), `test_hip_upload_race` fails on every run: 7 of 12 extractors off against the CPU in the pooled check (up to 8.3e2 on `float_moment`, 2.2 dB on `float_psnr`, 0.43 dB on `psnr`), and 8 of 272 scores changed in the shared check that refills both pictures the moment the frame ended. With the wait: 0 off, 0 changed.

## Dead ends

- A shared frame owned by the imported `VmafHipState` (the SYCL shape) was implemented first and replaced: one state can serve more than one context, and the planes belong to a frame, which a context has one of at a time.
- Keeping the motion twins' previous frame in a third slot of the shared frame instead of copying it: the copy is not visible in any run above, and a held previous frame would need its content preserved across skipped frames, which the two-slot fence does not have to do.

## Open questions

- A discrete AMD GPU, where an upload crosses PCIe and the runtime stages pageable copies itself, is unmeasured (`T-HIP-SHARED-UPLOAD-DISCRETE-GPU-2026-10-01`).
- `float_vif_hip` is not selected for `float_vif`, so `vmaf_float_v0.6.1` on HIP still computes VIF on the CPU; with it on the device the three float twins take 43 ms per 1080p frame where the model takes 47.
