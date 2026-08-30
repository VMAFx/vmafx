## Fixed

- **HIP wavefront-size (Bug 1)**: `integer_vif/vif_statistics.hip`,
  `float_vif_score.hip`, `float_motion_score.hip`, `float_psnr_score.hip`,
  and `moment_score.hip` all hardcoded the AMD wavefront size as 64, breaking
  RDNA2+ wave32 devices (gfx1030/gfx1100/gfx1101). VIF accumulator
  under-reduction caused ~25-pt VMAF score error. Fixed by using the HIP
  device variable `warpSize` at runtime and sizing shared-memory arrays for
  the minimum warp size (32) so they are large enough on both wave64 and
  wave32 hardware.

- **MCP probe frame too small (Bug 2)**: `mcp-server/vmaf-mcp/server.py`
  used a 32×32 probe YUV which is below the CUDA ADM 36px minimum, causing
  a silent null score that was incorrectly reported as `runtime_healthy=True`.
  Bumped probe resolution to 64×64 and tightened the health check to require
  `score is not None`.

- **libvmaf.c NULL deref in CUDA-only mode (Bug 3)**: When all registered
  feature extractors carry `VMAF_FEATURE_EXTRACTOR_CUDA` (device-only,
  `HW_FLAG_HOST` not set), `ref_host`/`dist_host` remain zero-initialised
  because `translate_picture_host()` early-returns. The subsequent
  unconditional `ref = &ref_host` then passes a zero-initialised picture to
  `threaded_read_pictures_batch` → `vmaf_ref_fetch_increment(NULL)`, a
  NULL-deref. Fixed with a guard: the host-reassignment is skipped when
  `hw_flags` does not include `HW_FLAG_HOST`.

- **ffmpeg-patches/0005 SYCL filter software-frame support (Bug 4)**:
  `libvmaf_sycl` accepted only `AV_PIX_FMT_QSV` hardware frames, making the
  filter unusable without a QSV decoder. Updated to
  `FILTER_PIXFMTS(AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_QSV)`.
  `do_vmaf_sycl` and `config_props_sycl` now branch on `frame->format ==
  AV_PIX_FMT_QSV`: QSV path keeps zero-copy VA surface import; software path
  allocates a `VmafPicture` and copies via `vmaf_read_pictures`.

- **Container missing Netflix golden YUVs (Bug 5)**: `dev/Containerfile` did
  not call `scripts/test/fetch-test-yuvs.sh`, so `pytest python/test/` always
  failed inside the container with missing fixture errors. Added a `RUN`
  layer to fetch and md5-verify the canonical src01 fixtures at image build
  time (ADR-0493).
