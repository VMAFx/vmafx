Fix dispatch-strategy registry gaps across SYCL, Vulkan, HIP, and Metal backends:

- Remove ~45 duplicate pointer entries from `feature_extractor_list[]` for SYCL
  (6 symbols doubled) and Vulkan (some symbols repeated up to 11 times).
- Replace the always-zero `vmaf_hip_dispatch_supports()` stub with a real
  `g_hip_features[]` table covering all 8 registered HIP extractors.
- Fix 8 wrong/missing feature-name strings in `vmaf_metal_dispatch_supports()`'s
  `g_metal_features[]` table; entries now match the actual `provided_features[]`
  arrays in `feature/metal/*.mm` (e.g. `"VMAF_feature_motion_score"` instead of
  `"float_motion"`, `"VMAF_integer_feature_motion2_v2_score"` instead of
  `"motion2_v2_score"`).
- Add `scripts/ci/check-dispatch-registry.sh` reproducer.
- Add dispatch-registry invariant notes to `core/src/hip/AGENTS.md` and
  `core/src/metal/AGENTS.md`.

Audit: `docs/research/0135-dispatch-strategy-registry-audit-2026-05-15.md`.
