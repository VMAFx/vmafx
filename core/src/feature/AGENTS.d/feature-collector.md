---
paths:
  - core/src/feature/feature_collector.cpp
  - core/src/feature/feature_collector.h
invariant: feature_collector.cpp mount/unmount traversal and single-authority lifecycle contracts.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# feature_collector.cpp Mount Traversal and Authority

- **`feature_collector.cpp` mount/unmount traversal**: fork rewrites
  `vmaf_feature_collector_mount_model` and `unmount_model` to walk
  local cursor instead of advancing pointer-to-head — upstream
  [Netflix#1406](https://github.com/Netflix/vmaf/pull/1406) is still
  OPEN as of 2026-04-20 and its body corrupts list on ≥3 mounted
  models. `unmount_model` additionally returns `-ENOENT` (not
  `-EINVAL`) for "model not mounted". If upstream ever merges #1406,
  **keep fork's version on conflict** — traversal is correct
  and errno split lets callers distinguish misuse from not-found.
  Test coverage in [`../../test/test_feature_collector.c`](../../../test/test_feature_collector.c)
  uses shared `load_three_test_models` / `destroy_three_test_models`
  helpers; upstream's PR inlines 60 LoC of per-model scaffolding that
  would trip clang-tidy `readability-function-size` (JPL-P10 rule 4).
  See [ADR-0132](../../../../docs/adr/0132-port-netflix-1406-feature-collector-model-list.md)
  and [rebase-notes 0031](../../../../docs/rebase-notes.md).
- **`feature_collector.cpp` is only implementation authority.** Commit
  `5d070b0b4` accidentally recreated C implementation after C++ migration,
  leaving production and tests on different bodies. Do not add
  `feature_collector.c` or point any build target at one. Preserve mutex
  coverage, full mounted-model snapshot, unlocked destroy traversal, unwind
  helpers, and `-EAGAIN` read contract together in C++ TU. fast
  `test_feature_collector_source_authority` gate fails if twin or stale
  build reference returns. See
  [Research-2100](../../../../docs/research/2100-feature-collector-source-authority-2026-09-24.md).
