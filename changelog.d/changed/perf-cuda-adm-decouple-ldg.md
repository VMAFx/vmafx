- **perf(cuda): adm_decouple — `__ldg()` F3 fix (ADR-0763)**

Extract `const T *__restrict__` band pointers from `AdmBufferCuda` before the
per-pixel body in `adm_decouple_kernel` (scale-0, `int16_t`) and
`adm_decouple_s123_kernel` (scales 1-3, `int32_t`). All six read-only loads
per kernel now use `__ldg()` to route through the L1 read-only texture cache.
Write-back side uses plain non-`const` pointers (no `__ldg()` on stores).
Mirrors the pattern established in ADR-0743 (VIF filter1d) and ADR-0754
(SSIM vert_combine / PR #93). The file is currently not compiled into the
active build; this is a preparatory maintenance change.
