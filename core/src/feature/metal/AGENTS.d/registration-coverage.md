---
paths:
  - core/src/feature/metal/*.mm
invariant: Every Metal extractor is registered and covered by registration contract test.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Registration coverage invariant

Every new Metal `VmafFeatureExtractor` added to
`core/src/feature/feature_extractor.cpp`'s `feature_extractor_list[]` must add
its basename to `core/test/test_metal_kernel_coverage_audit.c`'s
`g_metal_kernel_basenames[]` and update `EXPECTED_KERNEL_COUNT` in same
PR. Motion-class extractors must also appear in
`core/test/test_metal_kernel_registration.c`'s `kTemporal[]` table so
`VMAF_FEATURE_EXTRACTOR_TEMPORAL` scheduling flag is pinned. runtime-focused
`test_metal_smoke.c` is not authoritative registration inventory.

dedicated registration and 17-kernel audit tests supersede older
per-extractor smoke functions removed by stale squash; do not duplicate those
lookups back into runtime test. See Research-2091.
