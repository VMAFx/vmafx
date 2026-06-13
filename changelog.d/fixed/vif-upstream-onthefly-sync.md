- Netflix Golden D24 — full VIF on-the-fly filter sync from upstream
  Netflix (ADR-0416). Master CI was red on `feature_extractor_test.py`
  with `vif_num`/`vif_den` deltas of ~1e-5 on the canonical `src01` CPU
  pair after PR #754 reverted PR #723 (a partial port of upstream
  `bf9ad333`). PR #758 closes the half-port gap: takes upstream HEAD
  versions of `vif.{c,h}`, `vif_tools.{c,h}`, `vif_options.h` verbatim;
  `compute_vif()` gains an `int vif_skip_scale0` parameter; the only
  in-tree non-trivial caller (`float_vif.c::extract()`) threads it
  through. Adopts companion upstream test recalibrations: `142c0671`
  (VIF on-the-fly score values), `d93495f5` (libm tolerance loosen),
  `fe756c9f` (vifks360o97 tolerance), `7209110e` (motion_force_zero +
  param_neg_and_model_mfz score updates). VIF default and non-default
  kernelscale now both bit-match upstream Netflix; eliminates the
  fork's `vif_filter1d_table_s` lookup and `resolve_kernelscale_index`
  / `ALMOST_EQUAL` machinery.
