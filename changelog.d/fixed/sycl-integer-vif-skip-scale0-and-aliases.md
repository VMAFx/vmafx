- **`integer_vif_sycl` honours `vif_skip_scale0` at the emission site, and its
  option aliases match the CPU.** Three defects, all found by comparing the twin
  against the CPU extractor rather than by a failing test:
  1. With `vif_skip_scale0` set, the CPU never computes scale 0 and publishes
     `0.0` for `VMAF_integer_feature_vif_scale0_score`
     (`integer_vif.c::write_scale_scores`, not debug-gated). The SYCL kernel
     computes all four scales regardless — the flag only excluded scale 0 from
     the aggregate — so it published a real scale-0 ratio under a key the CPU,
     CUDA, HIP and Metal twins all report as `0`. SYCL was the sole outlier.
  2. `vif_skip_scale0` carried no `.alias`, where the CPU uses `ssclz`.
  3. `vif_enhn_gain_limit` carried no `.alias`, where the CPU uses `egl` — the
     option that defines the NEG model.
  Since [ADR-1183](docs/adr/1183-model-options-gate-gpu-twin-selection.md)
  derives the published feature name from the alias, (2) and (3) meant the twin
  filed its scores under names nothing else looks for. `test_sycl_vif_parity`
  gains `test_vif_skip_scale0_score_is_zero`, verified on an Arc A380 to fail
  before the fix and pass after.
