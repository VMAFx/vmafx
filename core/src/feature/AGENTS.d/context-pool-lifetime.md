---
paths:
  - core/src/feature/feature_extractor.h
  - core/src/feature/feature_collector.cpp
invariant: VmafFeatureExtractorContextPool fex_list growable pointer table lifecycle rules.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Feature-Context Pool Entry Lifetime Invariant

## Feature-context pool entry lifetime

`VmafFeatureExtractorContextPool::fex_list` is growable pointer table;
`get_fex_list_entry()` separately allocates each `fex_list_entry` and publishes
it only after `init_fex_list_slot()` succeeds. Do not move initialized entry:
`vmaf_fex_ctx_pool_aquire()` retains it while `pthread_cond_wait()` releases
pool mutex, and release must signal same condition-variable address.
entry also owns by-value snapshot of registered
`VmafFeatureExtractor`; never restore caller-owned descriptor pointer.
Callers may register stack descriptors, while CUDA/SYCL/frame-sync runtime
pointers are refreshed under pool lock before lazy context creation.
Preserve pointer-table and context-array size checks, and free each options copy
even if its first context allocation failed. Linux
`test_fex_pool_growth` regression forces table relocation while another
acquisition waits and mutates caller descriptor after registration. See
[pool-growth digest](../../../../docs/research/fex-pool-growth-2026-09-08.md).
