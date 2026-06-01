### perf(cuda): __ldg() bundle — #106 adm_decouple F3 fix (ADR-0763)

- **#106 APPLIED-CLEAN** — `adm_decouple_kernel` (scale-0, `int16_t`) and
  `adm_decouple_s123_kernel` (scales 1-3, `int32_t`) in
  `core/src/feature/cuda/integer_adm/adm_decouple.cu`: extract
  `const T *__restrict__` band pointers from `AdmBufferCuda` before the
  per-pixel body; convert all six read-only loads to `__ldg(&ptr[idx])`.
  Write-back uses plain non-`const` pointers. File is currently not compiled
  into the active build (decouple is inlined via `adm_decouple_inline.cuh`);
  this is a preparatory maintenance change. Mirrors ADR-0743 (VIF filter1d)
  and ADR-0754 (SSIM vert_combine). ADR-0763.
