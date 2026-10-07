---
paths:
  - core/src/feature/feature_collector.cpp
  - core/src/feature/feature_extractor.h
invariant: Non-finite values follow NaN and infinity cross-backend publication semantics.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Cross-Backend Non-Finite Publication Semantics

- **Cross-backend non-finite publication semantics (ADR-1302)**:
  `nonfinite_score.h` is shared validate-before-first-write seam for VIF,
  ADM, SSIM and MS-SSIM host code. CPU, CUDA, HIP, SYCL and Metal twins must
  validate every enabled output before clamp, fallback, dB conversion or
  collector append. Do not re-inline ordered comparisons in one backend: NaN
  takes fallback arm and becomes plausible score. All four VIF ratios are
  finite-checked before scale 0 is written, including integer and debug paths;
  only scales 1-3 then apply their configured minimum. Validate ADM reductions
  before precision floor, and validate every MS-SSIM L/C/S atom even when
  `enable_lcs=false`: otherwise comparison or `pow(NaN, 0)` can erase
  failure. Keep every registered host on seams checked by
  `test_nonfinite_collector_wiring.py`. Preserve ADR-1221's sole intentional
  non-finite output: finite perfect SSIM/MS-SSIM with dB enabled and clipping
  disabled reports positive infinity; invalid raw inputs and ceilings still
  fail frame.
