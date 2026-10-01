- **CUDA: `adm_cm.fatbin` register pressure and spill stack eliminated.**
  Restructured `adm_cm_aim_line_kernel` into adaptive launch bounds
  (`adm_cm_aim_line_kernel_2` and `adm_cm_aim_line_kernel_4`, ADR-1226) and fused
  scales 1-3 (`i4_adm_cm_aim_line_kernel_fused`), eliminating the 255-register
  ceiling and 344-byte spill stack (`STACK:0`, `LOCAL:0` across all architectures,
  `REG <= 176` on sm_89, max 208 on sm_100/120) with bit-identical scores and
  19-31% whole-feature speedups (`T-CUDA-ADM-CM-REGISTER-PRESSURE-2026-09-07`).
