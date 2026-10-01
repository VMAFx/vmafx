<!-- markdownlint-disable MD013 MD060 -->
# ADR-1408: A VmafContext uploads each plane of a frame once and every HIP twin reads that copy

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: hip, gpu, performance, rc3, fork-local

## Context

The HIP backend is host-picture only ([ADR-0530](0530-hip-feature-flag-promotion-and-picture-buffer.md)): a twin gets `VmafPicture` planes in pageable host memory and copies the ones it reads into device buffers of its own. A run with several twins therefore uploaded the same frame several times (`T-HIP-TWIN-PRIVATE-PLANE-UPLOADS-2026-09-29`). With the thirteen twins this change adopts in one process, a 4:2:0 frame pair was uploaded as 31 planes where it has 6.

Each of those uploads is a waiting one. `hipMemcpy2DAsync` can still be reading a pageable picture after `submit()` returned and the caller refilled it, so since `T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18` every twin stages through `vmaf_hip_picture_upload()`, which returns only once the copy has read the picture. On a device with one hardware queue the copy cannot start before the kernels queued ahead of it have left the GPU, so the host blocks once per twin and frame where it used to run ahead (`T-HIP-UPLOAD-WAIT-THROUGHPUT-2026-09-19`). The remedy that row proposed, a host copy into extractor-owned pinned memory, was adopted by five twins ([ADR-1377](1377-hip-motion-diff-first.md), [ADR-1378](1378-hip-cambi-device-resident.md), [ADR-1384](1384-hip-speed-device-resident.md)) and then measured slower than the wait for a single motion twin on the gfx1036, because that iGPU's runtime copies a pageable picture without a host copy.

SYCL settled the same question in [ADR-1369](1369-sycl-shared-planes-light-twins.md): the state uploads each plane once per frame and the twins read it.

## Decision

A `VmafContext` owns the device copy of the frame its HIP twins read: `VmafHipSharedFrame`, `core/src/hip/shared_frame.{c,h}`.

- **One upload per plane and frame.** `vmaf_read_pictures()` announces the frame's host pictures with `vmaf_hip_shared_frame_begin()` before the dispatch loop and ends the frame with `vmaf_hip_shared_frame_end()` after it. A twin asks for the planes it reads with `vmaf_hip_plane_source_acquire()` (or `_acquire_luma()`). The first request for a plane uploads it with `vmaf_hip_picture_upload()` on the asking twin's private stream; every later request in that frame gets the same device pointer and waits for nothing. An upload takes along every plane the twins asked for in the frame before, so a frame is normally one upload call and one host wait, whichever twin asks first. A plane no twin asked for in this frame or the one before is not uploaded, so a luma-only run uploads no chroma. Planes are packed (`width * bytes-per-sample` per row).
- **The pageable-upload race stays closed.** The pictures are read only between `begin()` and `end()`, inside the `vmaf_read_pictures()` call that owns them, and by the waiting upload. After `end()` the shared frame serves nothing from them.
- **Two slots, and a fence for the frame-skipping case.** Frames alternate between two slots. libvmaf collects a twin's frame N before it submits that twin's frame N + 1, and a twin's hold on a slot moves to the new slot when it acquires, so slot N % 2 has no reader left when frame N + 2 writes it. A twin that skipped a frame (`n_subsample`: the non-temporal twins) still holds the slot; the first upload into a held slot then waits for the device to go idle (`hipDeviceSynchronize()`), once per frame, so nothing is overwritten under a running kernel.
- **Fallback.** Without a shared frame (the extractor API used directly), outside `begin()` / `end()`, for a picture that is not the announced one, or for anything but a whole packed plane, the same call uploads into buffers the twin's `VmafHipPlaneSource` owns, as before.
- **Twins.** `psnr_hip`, `float_psnr_hip`, `float_moment_hip`, `ciede_hip`, `integer_ssim_hip`, `float_ssim_hip`, `vif_hip`, `float_vif_hip`, `adm_hip`, `float_adm_hip`, `motion_hip`, `motion_v2_hip` and `float_motion_hip` read the shared planes and no longer allocate picture staging. The motion twins need the previous frame, which the shared frame keeps only until the next but one: they keep one device plane, `prev_luma`, and replace it with a device-to-device copy enqueued behind the SAD on their stream. That replaces their pinned staging plane and their two-plane ping-pong.
- **Ownership.** The frame is created with the context's first HIP twin (`set_fex_hip_frame()`; a failed allocation leaves the twins on their own buffers) and destroyed by `vmaf_close()` after the extractors are closed. `VmafFeatureExtractor::hip_frame` hands it to a twin.

Three tests pin it: `test_hip_shared_frame` (the contract of `shared_frame.h` against stubs of the runtime, no device), `test_hip_shared_frame_contract.py` (the sources, with a planted regression per check) and `test_hip_upload_race` (on the device: every twin in one context against the CPU with and without frame subsampling, and both pictures refilled the moment the frame ended).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep one upload per twin | No change | 31 plane uploads and eleven waits where a frame has 6 planes | The row |
| The imported `VmafHipState` owns the shared frame, as the SYCL state does | Mirrors ADR-1369; planes survive across contexts | A state can be imported into more than one context: two contexts would share one frame's planes, and the planes' lifetime would hang on the caller's `vmaf_hip_state_free()` order. Twins selected by name run without a state and would not share | The context is what has one frame at a time; nothing in the public API changes |
| Pinned host planes (`hipHostMalloc`) the kernels read in place, filled by a host copy: no device copy and no wait | The host never waits on the device queue; documented as device-accessible in `hip_runtime_api.h` | Measured on the gfx1036 at 4K: `psnr` 7.06 -> 9.70 ms/frame, `psnr` + `motion_v2` 19.15 -> 22.00, `motion_v2` 12.48 -> 12.40 (the device upload: 10.86). The host copy of a pageable picture costs more than the runtime's upload | Slower on the only device measured |
| Staged upload for the shared planes (pinned staging, device copy without a wait, ADR-1377) | No wait | Same host copy as above plus the device copy; a single motion twin measured 13.24 ms/frame staged against 10.70 waiting (`T-HIP-UPLOAD-WAIT-THROUGHPUT-2026-09-19`) | Slower on the only device measured |
| Three slots, so the motion twins read the previous frame from the shared frame | No device-to-device copy | A third copy of every plane any twin asked for; a held previous frame needs its content kept, not only its kernels finished, so the fence is no longer enough when a frame is skipped | The copy is one enqueued command per frame, and both motion twins got faster with it |
| **Context-owned shared frame, waiting upload, two slots (chosen)** | One upload of each plane and normally one wait per frame; no host copy; the race stays closed by construction | A twin's planes are no longer its own: it must not write them and must not read them after its next acquire | Chosen |

## Consequences

Measured on `ryzen-4090-arc` (gfx1036, ROCm 7.2.4), `origin/master` 7dc45265f against the same tree with this change. [Research-1408](../research/1408-hip-shared-frame-planes.md) has the full tables.

- **Output is unchanged, bit for bit.** At `--precision max` every metric of every frame is identical before and after: the thirteen adopted twins in one process (`adm_hip`, `float_vif_hip` and `float_moment_hip` named, because `--backend hip` does not select the first two for `adm` and `float_vif`), also with `--subsample 2`, `--model version=vmaf_v0.6.1`, `--model version=vmaf_float_v0.6.1` and the row's `psnr` + `psnr_hvs` + `motion_v2`, on the Netflix 576x324 pair (48 frames, and 3 frames at 10 bits), both 1080p checkerboard pairs (3 frames each) and BBB 3840x2160 (50 frames): 390 metric series, every frame identical.
- **Uploads per frame**: thirteen twins 31 -> 6 planes and 11 -> 1 host waits; the default model (`motion_hip` + `vif_hip`, ADM on the CPU) 3 -> 2 planes, one wait as before; `vmaf_float_v0.6.1` (`float_adm_hip` + `float_motion_hip`, `float_vif` on the CPU) 3 -> 2 planes and 2 -> 1 waits.
- **Throughput**, ms per frame in steady state inside one process, median of interleaved runs of the two builds, load average 6 to 35 from other jobs:
  - `--model version=vmaf_float_v0.6.1`: 57.30 -> 46.86 at 1920x1080 (17.5 -> 21.3 frames per second, seven pairs) and 294.32 -> 226.43 at 3840x2160 (five pairs, the change faster in each). This is the configuration `T-HIP-UPLOAD-WAIT-THROUGHPUT-2026-09-19` measured losing 21% to the per-twin wait: `float_motion_hip` no longer waits behind `float_adm_hip`'s kernels, so the host reaches the frame's CPU `float_vif` while they run.
  - `--model version=vmaf_v0.6.1` (the default model): 37.36 -> 37.37 at 1080p (nine pairs); at 4K both builds run at about 155 or about 230 ms depending on what else loads the host, with no difference between them.
  - runs entirely on the device are bound by their kernels and do not move: thirteen twins 182.71 -> 184.20 at 1080p and 731.17 -> 734.96 at 4K, `adm_hip` + `vif` + `motion` 52.87 -> 52.51 at 1080p.
  - light runs at 4K: `motion` 12.39 -> 11.04 and `motion_v2` 12.41 -> 10.95 (no host copy into pinned staging any more), `psnr` + `float_psnr` + `float_moment_hip` 17.16 -> 15.11, `psnr` + `motion_v2` 18.49 -> 17.96, `psnr` 7.14 -> 7.35 (inside its spread).
  - the row's command, `psnr` + `psnr_hvs` + `motion_v2`: 8.00 -> 7.97 at 1080p, 31.74 -> 32.27 at 4K (seven pairs, inside its spread); `psnr_hvs_hip` is most of it and still stages its own planes.
- **Negative**: a twin's device planes belong to the context. A twin that writes them, or reads them after its next acquire, corrupts another twin; `shared_frame.h` states the rules and the contract test checks what can be checked statically. The first twin of a frame still waits for its upload; on this iGPU that is cheaper than avoiding it.
- **Neutral / follow-ups**: `psnr_hvs_hip` and `ssimulacra2_hip` belong to changes in review and are not adopted here; `float_ms_ssim_hip` converts to `float` on the host, and `cambi_hip` and the SpEED twins stage through pinned memory. `T-HIP-SHARED-FRAME-REMAINING-TWINS-2026-10-01` tracks them. A discrete AMD GPU, where the upload crosses PCIe and the runtime stages pageable copies itself, is unmeasured; the shared frame is the one place to change the upload for it.

## References

- req: RC3 HIP lane brief (2026-10-01): "T-HIP-TWIN-PRIVATE-PLANE-UPLOADS-2026-09-29 and T-HIP-UPLOAD-WAIT-THROUGHPUT-2026-09-19: every HIP twin uploads its own copy of the planes and the upload wait costs throughput; share the uploaded planes across twins per frame (the SYCL precedent is ADR-1369 shared planes) without reintroducing the pageable-upload race that the wait fixed (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18; keep test_hip_upload_race meaningful). Measure a multi-twin run (default model) before/after."
- [ADR-1369](1369-sycl-shared-planes-light-twins.md) — the SYCL precedent.
- [ADR-0530](0530-hip-feature-flag-promotion-and-picture-buffer.md) — host pictures on HIP.
- [ADR-1377](1377-hip-motion-diff-first.md) — the motion SAD pipeline and the staged upload this replaces for the motion twins.
- [Research-1408](../research/1408-hip-shared-frame-planes.md) — measurements, including the rejected upload variants.
