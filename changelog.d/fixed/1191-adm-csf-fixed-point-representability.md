- **Integer ADM no longer emits wrapped scores for out-of-range CSF
  configurations** (`core/src/feature/integer_adm.c` plus the CUDA, HIP and
  SYCL twins). The fixed-point pipeline stores each scale's
  contrast-sensitivity weight in a `uint16_t` (scale 0) or `uint32_t`
  (scales 1-3), sized for the Watson97 weights. `--feature adm=adm_csf_mode=1`
  (Barten) at the default `adm_csf_scale` produces weights 38x to 155x past
  those ceilings; the narrowing casts wrapped silently and the extractor
  emitted `integer_adm2_csf_1: null` with per-scale scores three orders of
  magnitude below the float reference. The blended-CSF modes (`2` / `3`) had a
  related defect: for viewing geometries their tables do not carry they return
  `-EINVAL` *as a float*, which then hit an undefined negative-to-unsigned
  conversion. Both configurations are now rejected with `-EINVAL` and a log
  line naming the scale, band and offending weight. Configurations that fit
  are unchanged, including `adm_csf_mode=1` with small `adm_csf_scale` /
  `adm_csf_diag_scale` coefficients and `adm_csf_mode=2` as requested by the
  default model `vmaf_v1.0.16_3d0h`; the Netflix golden gate is untouched.
  Use `--feature float_adm` to run the Barten CSF at full scale.
  ADR-1191, `docs/metrics/features.md` § Fixed-point CSF limits.
