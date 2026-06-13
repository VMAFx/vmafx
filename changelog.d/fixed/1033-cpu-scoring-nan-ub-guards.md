### Fixed

- **integer_psnr.c APSNR flush**: guard `log10(0)` NaN when SSE == 0
  (identical frames) by emitting `psnr_max[i]` as the clamped ceiling; bound
  loop to `1` plane when `enable_chroma=false`; remove accidental `* 2`
  inflation in the `max_apsnr` cap formula (ADR-1033).
- **ms_ssim.c**: wrap `l` and `c` in `fabs()` before `pow()` to prevent
  `pow(negative, fractional)` NaN on heavily distorted frames; cast each
  `w * h` operand to `double` before multiply to prevent signed int32 overflow
  for images wider than 46340 pixels (ADR-1033).
- **float_ssim.c / float_ms_ssim.c `convert_to_db`**: guard `score >= 1.0`
  before `log10(1 - score)` to return `max_db` rather than NaN/-Inf
  (ADR-1033).
- **iqa/ssim_tools.c**: replace `assert(!args)` with a runtime
  `if (args && !mr) return INFINITY` guard so non-default `args` callers
  (the Rouse MS-SSIM path) do not abort the process (ADR-1033).
- **adm.c / integer_adm.c**: initialise `*score_aim = 1.0f` in the
  `den == 0` flat/black frame branch; previously uninitialized, causing
  garbage AIM scores for all-black sequences (ADR-1033).
- **float_adm.c harmonic mean**: guard `score + score_aim == 0.0` to return
  `0.0` instead of `0/0` NaN which `MAX()` silently propagates (ADR-1033).
- **float_adm.c `adm_skip_scale0`**: emit an explicit `0.0` for scale-0 when
  the skip flag is set, mirroring the `float_vif.c` guard (ADR-1033).
- **motion.c bilinear stride**: pass the caller-supplied `img1_stride` /
  `img2_stride` to `motion_scale_bilinear` instead of recomputing from
  `width`; the recalculated value ignored any alignment padding in the
  original allocation and read incorrect rows (ADR-1033).
- **motion.c OOM**: add NULL guards after both `aligned_malloc` calls for the
  scale-1 downsampled buffers; on OOM return the already-computed
  `motion_scale0` gracefully (ADR-1033).
- **cambi.c `v_band_size` overflow**: compute band size in signed int and
  return `-EINVAL` with a diagnostic when the result is non-positive, preventing
  `uint16_t` wrap-to-~65535 and the resulting massive over-alloc + OOB writes
  (ADR-1033).
