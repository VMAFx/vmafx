- HIP backend: removed the weak `float_vif_score_hsaco` HSACO stub
  from `core/src/feature/hip/hip_hsaco_stubs.c` (ADR-0539). The real
  HIP kernel at `core/src/feature/hip/float_vif/float_vif_score.hip`
  has been the strong definition since ADR-0379 / PR #1025; the stub
  was dead code left over from PR #1303's blanket weak-symbol
  extension and was emitting an `-Wlto-type-mismatch` warning on every
  `enable_hipcc=true` build. No runtime behaviour change.
