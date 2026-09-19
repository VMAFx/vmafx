- **HIP extractors no longer score frames against the next frame's samples.**
  HIP pictures are ordinary host memory, and eleven HIP extractors uploaded
  them with an asynchronous copy and returned without waiting for it. The
  caller then refilled the picture with the next frame while the copy was
  still reading. On a multi-frame run with `--backend hip`, `ciede_hip`,
  `float_adm_hip`, `float_moment_hip`, `float_psnr_hip`, `float_ssim_hip`,
  `float_vif_hip`, `psnr_hip` and `vif_hip` reported wrong values for some
  frames (up to 10.7 dB on `float_psnr`, 0.30 on `vif`, over the 48-frame
  Netflix pair): a different set of frames on every run, or, with several
  extractors in one process, the same 46 of 48 frames on every run.
  `float_motion_hip`, `motion_hip` and `motion_v2_hip` had the same defect but
  could not show it through `vmaf_read_pictures()`. All of them now wait for
  their uploads, through one shared helper. Recompute any multi-frame score
  taken from these extractors before this release; a single-frame score was
  never affected (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18).
- **HIP throughput on small AMD GPUs can be lower with the float model.** The
  wait above costs `--model version=vmaf_float_v0.6.1` about 21 % at 1080p on
  a gfx1036 iGPU. `vmaf_v0.6.1` and single extractors are unchanged within
  noise. Tracked as T-HIP-UPLOAD-WAIT-THROUGHPUT-2026-09-19.
