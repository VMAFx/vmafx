- CUDA build failure: removed duplicate `inline_i4_csf_r` and `inline_s0_csf_r`
  definitions in `core/src/feature/cuda/integer_adm/adm_cm.cu` introduced when
  PR #565 (adm_decouple `__ldg()` F3 fix) was admin-merged while master already
  contained those helpers in the AIM CM block. The `__ldg()` version (lines
  437/465) is retained; the old non-`__ldg()` duplicate block (previously lines
  742–1055) is removed. Unblocks Docker Image Build + CUDA build matrix.
