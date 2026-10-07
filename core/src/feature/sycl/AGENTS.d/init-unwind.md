---
paths:
  - core/src/feature/sycl/integer_motion_sycl.cpp
  - core/test/test_sycl_init_unwind.cpp
invariant: A failed extractor init owns its cleanup; calls NULL-safe local close callback.
---
<!-- markdownlint-disable MD013 MD060 -->
# Failed extractor init cleanup

- **failed extractor `init` owns its cleanup (BUG-048 section E).**
  generic feature-extractor framework does not invoke `close` after `init`
  returns error. Every SYCL init path that has acquired USM, feature-name
  dictionary, or graph registration must therefore call its NULL-safe local
  close callback before propagating error. This is enforced without GPU
  by `core/test/test_sycl_init_unwind.cpp`; keep allocator, dictionary, and
  graph fault cases when rebasing any init/close pair. Historical producer
  `709ce470e` was reverted by `5d070b0b4`; current restoration boundary is
  documented in
  `docs/research/2101-bug048-sycl-init-unwind-restoration-2026-09-24.md`.
