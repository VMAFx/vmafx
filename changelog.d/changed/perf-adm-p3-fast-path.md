- **Restore `adm_sum_cube_s_p3`, `adm_csf_den_scale_s_p3`, and `adm_cm_s_p3` fast-path
  functions in `adm_tools.c` (ADR-0463 / BUG-048 B3).**
  The specialized `adm_p_norm == 3.0` fast-paths eliminate all per-pixel `powf()`
  calls and branch overhead on the hot path for default VMAF evaluation.
  Scores remain 100% bit-identical to the baseline generic path across all 48
  frames on the Netflix 576x324 reference pair at `--precision max`.
  Dispatched once per scale in `core/src/feature/adm.c`.
