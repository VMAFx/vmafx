### Fixed

- **CUDA feature-extractor cleanup, motion precision, and error-code fidelity
  (T-BUGHUNT-CUDA-2026-06-27)**: Three classes of CUDA-backend bugs found by
  the 2026-06-27 bug-hunt sweep, all fork-local and none touching the Netflix
  CPU golden gate.
  - **Pinned host-buffer leaks** in `float_vif_cuda` (`num_host[4]` /
    `den_host[4]`) and `float_adm_cuda` (`accum_host[FADM_NUM_SCALES]`): the
    device buffers were freed in `close_fex_cuda` and the init `free_buffers`
    error path, but the page-locked host buffers allocated via
    `vmaf_cuda_buffer_host_alloc` were never released, leaking on every
    `vmaf_close()`. Added `vmaf_cuda_buffer_host_free` calls in both the close
    and init-error paths. (`integer_ms_ssim_cuda` was already correct — its
    close already frees `h_ref` / `h_cmp` / the per-scale partial triples, so
    no change there.)
  - **integer_motion_cuda SAD precision**: `normalize_and_scale_sad` truncated
    to single precision via an intermediate `(float)` cast and divided by an
    unsigned `w * h` product, diverging from the CPU reference
    (`(double)sad / 256. / (w * h)` in `integer_motion.c`). Recast end-to-end
    in double precision to match the CPU twin and motion_v2.
  - **speed_temporal / speed_chroma CUDA error-code squash**: every `fail:`
    label reached from `CHECK_CUDA_GOTO` returned a literal `-EIO`, discarding
    the `_cuda_err` value the macro had already mapped from the CUresult
    (`-ENOMEM` / `-ENODEV` / `-EINVAL` / `-EIO`). Changed all macro-reachable
    `fail:` / `fail_pop:` / `fail_after_pop:` labels to `return _cuda_err;`,
    matching the `CHECK_CUDA_RETURN` convention; the two manual
    `cuMemcpyDtoH` / `cuCtxPushCurrent` boolean checks keep their literal
    `-EIO`.

  Reproducer / verification: built with `-Denable_cuda=true` (CUDA 13.3,
  RTX 4090) and ran the GPU test suite. The touched-feature parity / smoke /
  leak tests all pass: `test_cuda_float_vif_parity`,
  `test_cuda_float_motion_parity`, `test_cuda_motion3_parity`,
  `test_cuda_motion_v2_parity`, `test_cuda_speed_temporal_{smoke,parity}`,
  `test_cuda_speed_chroma_{smoke,parity}`, `test_cuda_preallocation_leak`,
  `test_cuda_pic_preallocation`. (Pre-existing, unrelated parity failures in
  `float_adm` / `cambi` / `ssimulacra2` / `float_moment` / `psnr_hvs` were
  confirmed present on the unmodified baseline and are out of scope.)
