### CUDA ADM decouple-inline — `__ldg()` read-only cache routing on active path (ADR-0773)

Apply F3 fix to the live CSF / CM kernel path that executes every ADM frame dispatch:

- `adm_csf.cu`: extract `const T *__restrict__` band pointers before the inner loops in
  `i4_adm_csf_kernel<>` (int32) and `adm_csf_kernel<>` (int16); replace all 6 indexed
  reads with `__ldg()`.
- `adm_cm.cu`: same extraction in all six inline device helpers:
  `inline_i4_csf_a`, `inline_i4_decouple_r`, `inline_s0_csf_a`,
  `inline_s0_decouple_r`, `inline_i4_csf_r`, `inline_s0_csf_r`.
- Routes DWT2 band loads through L1 read-only texture cache, reducing L2 pressure at
  resolutions where the planes exceed L1 capacity (1080p and above).
- CUDA vs CPU correctness: places=4 PASS; max diff ≤ 1.00e-06 on Netflix 576×324
  reference pair, 0.00e+00 on 1080p checkerboard.
- `adm_decouple.cu` (dead code, ADR-0763) was addressed separately in PR #106;
  this PR addresses the active inline path.
