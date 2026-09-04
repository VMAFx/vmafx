### Fixed

- `core/src/feature/integer_adm.c`: reject CSF configurations whose scale-0
  factors exceed the 16-bit fixed-point budget (`factor1 * 2^21 >= 65536` or
  `factor2 * 2^23 >= 65536`) with `-EINVAL` and an error log naming the offending
  mode and values, preventing silent score distortion (e.g. `adm_csf_mode=1`
  Barten at default 1080p/3H viewing conditions). Documented in `docs/metrics/features.md`
  and ADR-1174. Stage 1 of T-UPSTREAM-1494; stage 2 will widen the internal pipeline.
