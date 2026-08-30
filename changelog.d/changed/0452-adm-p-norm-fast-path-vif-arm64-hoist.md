- **perf(adm):** Split `adm_p_norm == 3.0` inner-loop branch into dedicated
  fast-path functions (`adm_cm_s_p3`, `adm_csf_den_scale_s_p3`,
  `adm_sum_cube_s_p3`) dispatched once in `compute_adm`. Eliminates 8+
  per-pixel `if`/`powf` branches on the default code path; enables
  auto-vectorisation of the cube accumulation loops. Bit-exact to the
  generic path when `adm_p_norm == 3.0` (cbrtf == powf(x,1/3) for finite
  non-negative x; accumulation order preserved). (ADR-0463)
- **perf(vif):** Remove per-call `aligned_malloc` from the scalar fallback
  paths of `vif_filter1d_s`, `vif_filter1d_sq_s`, and `vif_filter1d_xy_s`.
  These functions already receive a correctly-sized `tmpbuf` from their
  caller (`vif.c`); the scalar fallback now reuses it instead of
  allocating a fresh buffer on every call. On ARM64 and non-AVX2 x86 this
  eliminates 12 `aligned_malloc`/`aligned_free` pairs per frame per VIF
  scale. No output change. (ADR-0463)
