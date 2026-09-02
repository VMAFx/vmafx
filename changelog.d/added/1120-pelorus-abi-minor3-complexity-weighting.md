Re-pinned the vendored Pelorus interop ABI mirror from `pelorus@835e097`
(ABI 1.0) to `pelorus@818d844` (ABI 1.3, ADR-1120) and taught perceptual
weighting to consume the new per-frame `PEL_SEC_COMPLEXITY` section. The mirror
gains three vendored files (`pelorus/denoise.h`, `pelorus_denoise_params.c`,
`pelorus_qp_report_csv.c`); the minor-3 conformance fixture
(`test_pelorus_interop`, now 14 vectors) links the x265 CSV QP-report reader.
Perceptual weighting (`--lavfi libvmaf=perceptual_weight=1`, C-API
`vmaf_set_perceptual_*`) now attenuates the banding salience by
`(1 − 0.5·complexity)` (floored at 0.25): banding is up-weighted on flat/simple
frames and masked on busy/textured frames. The modulation is opt-in and
golden-isolated — an absent complexity section means an exact `1.0` factor, so
the Netflix golden 576×324 pair still scores `76.667831`. Also fixes the
`scripts/sync-pelorus-interop.sh --update` bug that re-vendored the six manifest
files but not the conformance-fixture body. See `docs/api/pelorus-interop.md`
and `docs/api/perceptual-weight.md`.
