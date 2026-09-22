- HIP integer ADM: the four kernels that read `AdmBufferHip`
  (`adm_csf_kernel_1_4`, `i4_adm_csf_kernel_1_4`, `i4_adm_cm_line_kernel`,
  `adm_cm_line_kernel_8`) take it by pointer again, as ADR-0759 decided.
  The decision landed in `31a51afb2` (#101) and was undone the same day by
  `92ea978a4` (#102), a change cut from an older base; the invariant note in
  `core/src/feature/hip/AGENTS.md` survived the revert, so the file documented
  a contract the kernels no longer held. The host uploads one device copy of
  the struct at init and passes its address. Measured with ROCm 7.2 on
  `gfx1036`, `gfx1100` and `gfx90a`: 320 bytes off every launch's kernel
  arguments, and the scale-0 CM kernel's per-thread scratch drops with it
  (920 to 608 bytes on `gfx1036`) because the by-value copy was being
  spilled. Output is bit-identical before and after on a real `gfx1036`
  device for all four kernels.
