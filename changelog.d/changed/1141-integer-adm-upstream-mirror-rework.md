- The upstream-mirror integer ADM (`core/src/feature/integer_adm.c`) and the
  float ADM tools (`core/src/feature/adm_tools.c`) are now lint-clean to the
  fork's strictest clang-tidy profile and to cppcheck with every kernel
  expression, integer width, rounding term and summation order kept verbatim:
  the eighteen `ADM_CM_THRESH_S_*` corner / edge / interior macros collapse
  into mirrored-neighbourhood helpers, `adm_cm` / `i4_adm_cm` / `adm_cm_s` /
  the DWTs / `integer_compute_adm` / `init` / `extract` split along their
  phase boundaries, 24 NOLINT sites shrink to four cited suppressions, two
  dead functions (`adm_dwt2_lo_d`, `adm_buffer_copy`) and two latent-UB sites
  (an unguarded `pow(2, shift - 1)` rounding term, an unreachable
  out-of-bounds `rfactor[]` read) are gone. Netflix golden scores, the
  AVX2 / AVX-512 / NEON twins and every CLI option path are bit-identical
  (62-run `--precision max` output matrix, 21 396 metric values). (ADR-1141)
