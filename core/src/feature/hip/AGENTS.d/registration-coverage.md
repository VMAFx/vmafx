---
paths:
  - core/src/feature/feature_extractor.cpp
  - core/src/feature/hip/integer_adm_hip.c
invariant: Every new HIP feature extractor must be registered in feature_extractor.cpp under HAVE_HIP.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Registration coverage invariant

Every new HIP `VmafFeatureExtractor` added to
`core/src/feature/feature_extractor.cpp`'s `feature_extractor_list[]` must have
`vmaf_get_feature_extractor_by_name()` assertion in
`core/test/test_hip_smoke.c` and entry in that file's `test_table[]` in
same PR. Motion-class extractors must additionally pin
`VMAF_FEATURE_EXTRACTOR_TEMPORAL`. Raw kernel-stub helpers that expose only
`vmaf_hip_<name>_init` / `_run` / `_destroy` without
`VmafFeatureExtractor` descriptor are exempt until promotion. See
Research-2091.
