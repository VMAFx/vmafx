# fix(feature): restore Netflix golden VMAFEXEC score on AVX-512 CPUs (ADR-1104)

Remove AVX-512 dispatch from `vif_filter1d_s`, `vif_filter1d_sq_s`, and
`vif_filter1d_xy_s` in `core/src/feature/vif_tools.c`.

**Root cause**: ADR-0504 added AVX-512 float VIF convolution, but the wider
FMA partial-sum tree (16 floats vs 8 in AVX2) produces different IEEE-754
rounding. On AVX-512 CPUs the VMAFEXEC mean score for the Netflix src01 pair
was approximately `76.66729`, which exceeds the `places=4` (5e-5) tolerance
against the golden assertion `76.66740433333332`. The regression was latent
because GitHub Actions runners do not expose AVX-512.

**Fix**: Float VIF now dispatches to AVX2 (or scalar), matching the upstream
Netflix/vmaf behavior. The integer VIF AVX-512 path (`vif_avx512.c`) is
unaffected.

**Verified**: 271 passed / 12 skipped / 0 failed across vmafexec_test,
vmafexec_feature_extractor_test, quality_runner_test, feature_extractor_test,
result_test, and ssimulacra2_test.
