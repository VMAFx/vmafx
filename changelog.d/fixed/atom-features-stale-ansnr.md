Remove stale `"ansnr"` and `"anpsnr"` entries from `VmafFeatureExtractor.ATOM_FEATURES`
and `VmafIntegerFeatureExtractor.ATOM_FEATURES_TO_VMAFEXEC_KEY_DICT`. Both entries
mapped to `float_ansnr` / `float_anpsnr` which were removed in PR #38
(Research-0733 feature importance audit). Any consumer walking `ATOM_FEATURES`
would previously receive keys that resolve to non-existent C extractors on CPU,
CUDA, SYCL, HIP, and Metal backends (Vulkan-only survivor). Resolves
Research-0733 Phase 2 follow-up flagged by PR #87.
