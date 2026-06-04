### Fixed

- **CUDA VIF `filter1d.cu` 16-bit vertical rd-filter upper-bound typo** — the
  upper-bound guard in the 16-bit vertical kernel wrote `fwidth_rd - fwidth_rd`
  (always zero) instead of `fwidth - fwidth_rd`, causing the rd-filter window
  to span all `fwidth` taps and index `vif_filt.filter[scale+1]` 1–4 entries
  beyond allocation. Result: OOB reads and wrong VIF scores at scales 0–2 on
  the CUDA backend. Fix mirrors the correct 8-bit form at line 183
  (`fi < (fwidth - (fwidth - fwidth_rd) / 2)`).
  File: `core/src/feature/cuda/integer_vif/filter1d.cu`.

- **CUDA integer ADM CM operator-precedence bug (×2)** — two reduction loops in
  `integer_adm/adm_cm.cu` (lines 373 and 712) computed `x_sq` as
  `+ add_shift_sq >> shift_sq` (parsed as `+ (add_shift_sq >> shift_sq)` = `+ 0`
  because `>>` binds tighter than `+`), silently dropping the shift normalisation.
  `x_sq` carried an un-normalised squared value (~10^6 instead of ~0–1), causing
  int32 overflow and wrong ADM scale-0 and AIM scores on the CUDA backend.
  Fix adds the required parentheses: `((int64_t)accum * accum + add_shift_sq) >> shift_sq`,
  matching the CPU reference macro `I4_ADM_CM_ACCUM_ROUND` at `integer_adm.c:743`
  and the correct fused kernel at line 259.
  File: `core/src/feature/cuda/integer_adm/adm_cm.cu`.
