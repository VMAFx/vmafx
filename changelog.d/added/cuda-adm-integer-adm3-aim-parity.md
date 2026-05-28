`integer_adm_cuda` (the default CUDA ADM extractor) now emits
`VMAF_integer_feature_aim_score` and `VMAF_integer_feature_adm3_score`,
closing the feature-parity gap with the CPU `integer_adm` extractor.

Consumers of `pooled_metrics` requesting these features with `--backend cuda`
no longer receive NaN / missing values.

Two new options are exposed: `adm_skip_aim` (default `false`) and
`adm_dlm_weight` (default `0.5`), matching the CPU extractor defaults.

**ADR-0746** — covers the implementation (fully-inlined AIM CM kernels,
no new device buffers, `RES_BUFFER_SIZE` 24 → 36).
