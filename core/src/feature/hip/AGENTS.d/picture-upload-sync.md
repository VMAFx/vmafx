---
paths:
  - core/src/feature/hip/integer_adm_hip.c
  - core/src/feature/hip/integer_ssim_hip.c
invariant: Never return from submit while picture upload transfer is in flight.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Picture uploads: never return from submit() with one in flight

**Invariant: extractor must not return from `submit()` while upload from
pooled host picture is in flight.**

HIP pictures = pageable host memory; no HIP picture pool yet (T7-10c).
`hipMemcpy2DAsync` is asynchronous with respect to host: bare call can still
read `VmafPicture::data` after `submit()` returned, and caller may refill
picture at once. CLI pool is LIFO: distorted picture of frame N = first buffer
refilled for frame N + 1. Result: frames scored against next frame's samples.
Different set on every run. With several extractors in one process: same wrong
46 of 48 frames on every run (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18).

Rules:

- Twin reads frame planes -> ask context's shared frame (ADR-1408,
  `core/src/hip/shared_frame.h`): `vmaf_hip_plane_source_acquire_luma()` for
  ref + dis luma, `vmaf_hip_plane_source_acquire()` for any set of whole
  planes; `fex->hip_frame` + `VmafHipPlaneSource` in private state;
  `vmaf_hip_plane_source_close()` in close after stream is drained. First
  twin of frame that asks uploads plane (waiting upload, on its own
  stream) plus every plane any twin asked for in frame before; every
  other twin gets same device pointer, uploads nothing, waits for
  nothing -> normally one wait per frame. No own `hipMalloc` staging for picture planes, no
  `vmaf_hip_picture_upload()` in twin. Adopted: `psnr_hip`, `float_psnr_hip`,
  `float_moment_hip`, `ciede_hip`, `integer_ssim_hip`, `float_ssim_hip`,
  `vif_hip`, `float_vif_hip`, `adm_hip`, `float_adm_hip`, `motion_hip`,
  `motion_v2_hip`, `float_motion_hip`.
  Not yet: see T-HIP-SHARED-FRAME-REMAINING-TWINS-2026-10-01 in
  `docs/state.md`.
- Shared plane = read-only, packed (`width * bytes-per-sample` per row), valid
  until twin's next acquire or close. Frames alternate between two slots;
  libvmaf collects frame N - 1 of twin before its submit N, so slot N % 2 is
  free again at frame N + 2. Twin that needs frame N's plane at frame N + 1
  keeps copy: motion twins copy device-to-device into `prev_luma` behind
  SAD on their stream (`integer_motion_sad_hip.c`).
- Pictures are read only between `vmaf_hip_shared_frame_begin()` and
  `vmaf_hip_shared_frame_end()`, which `vmaf_read_pictures()` puts around
  dispatch loop. Do not move `end()`, do not acquire outside pair:
  caller refills pictures when `vmaf_read_pictures()` returns.
- Twin without shared frame (extractor API used directly, picture not
  announced one, part of plane) uploads into own buffers inside same
  call, with same wait. Nothing for twin to do.
- Extractor-owned pinned staging stays valid for twins that convert or pack
  on host: `vmaf_hip_picture_upload_staged()` (host copy into
  `vmaf_hip_picture_staging_alloc()` buffer before return, device copy from
  it, no wait; `cambi_hip`, SpEED twins, ADR-1378, ADR-1384) and float
  staging of `integer_ms_ssim_hip` / `integer_psnr_hvs_hip`. Buffer reuse next
  frame safe only because `collect()` drains stream copies ran on and
  libvmaf collects frame N - 1 before submit N. Do not call
  `hipMemcpy2DAsync` / `hipMemcpyAsync` on picture plane directly.
- Upload stream = extractor's private stream (`lc.str`), even when kernels
  run on null stream. Null-stream copy queues behind every null-stream kernel
  of frame; wait then blocks host on all of them. Copy completes before any
  kernel is enqueued -> no cross-stream ordering needed. About ordering
  guarantee, not speed: on gfx1036 one hardware queue serialises copy behind
  running kernels either way.
- Reference picture looks safe, is not. `vmaf_read_pictures()` keeps it alive
  two more frames through `prev_ref` / `prev_prev_ref` (one before ADR-1478);
  hence `motion_hip`, `motion_v2_hip`,
  `float_motion_hip` never misbehaved under CLI. libvmaf implementation
  detail, not contract: through extractor API their copy was still reading
  after `submit()` on 10 of 10 runs.
- Single-frame fixture cannot see any of this; neither can determinism check
  alone. `core/test/test_hip_upload_race.c` covers every uploading extractor
  four ways: pooled frames against CPU (`hip_pooled_fixture.h`), both pictures
  refilled moment `submit()` returns (bit-identical scores), and both
  again with every extractor on one shared frame, with and without
  `n_subsample`. Add new extractor to its `race_cases[]` table.
  `core/test/test_hip_shared_frame.c` checks shared-frame contract
  without device.

Device pictures (VMAFx imports, ADR-2092): the same calls copy device to
device on the picture's library stream, no host wait; the twin's stream and
the null stream wait on an event. A twin that stages on the host
(`integer_psnr_hvs_hip`, `ssimulacra2_hip`, `integer_ms_ssim_hip` level 0)
must branch on `vmaf_hip_picture_device_stream()` and copy / convert on the
device; `core/test/test_vmafx_import_hip_contract.py` holds the branch order.

Wait costs host time, normally once per frame instead of once per twin since
ADR-1408.
Numbers: `docs/backends/hip/overview.md` "Picture uploads". Pinned planes
kernels read in place and pinned staging both measured slower on gfx1036
(Research-1408); discrete AMD GPU unmeasured. Do not buy throughput back by
dropping wait.
