---
paths:
  - core/test/test_feature_isa_invariance.c
invariant: Feature scores must not depend on host ISA; promotions and reductions must match scalar.
area: simd-and-gpu
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Feature Score Invariance Across Host ISAs

## A score must not depend on the host ISA (ADR-1207, ADR-1208)

Every SIMD kernel in this tree is required to be bit-exact with its scalar
reference. Same input must produce same bits on AVX-512 host,
AVX2 host and host with no SIMD. Two things follow for anyone touching
kernel here:

- per-feature `test_<feature>_simd.c` files compare SIMD kernel against
  scalar reference **defined inside test TU**, because shipped scalar
  functions are `static`. That reference can drift away from shipped one —
  it did, twice (ADR-1205, ADR-1208). Passing `test_<feature>_simd` is
  therefore necessary but not sufficient.
- `core/test/test_feature_isa_invariance.c` is gate that compares
  shipped SIMD path against shipped scalar path, end to end, via
  `VmafConfiguration.cpumask`. Run it after any kernel change.

Concretely, when kernel promotes `float` inputs to `double`, do promotion
**before** arithmetic, not after. `(double)a - (double)b` is exact for two
floats; `(double)(a - b)` is not, and mixing two between vector body and
its scalar tail makes result depend on vector width.
