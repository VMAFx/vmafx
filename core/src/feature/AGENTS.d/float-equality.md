---
paths:
  - core/src/feature/feature_name.cpp
  - core/src/feature/brisque_math.h
invariant: Explicit floating-point comparison contracts replace direct equality tests.
area: framework
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Floating-Point Equality and Comparison Contracts

## Floating-point equality contracts (ADR-1308)

CodeQL flags direct float equality (`==` / `!=`). In this subtree:

- In `feature_name.cpp`, `option_double_equals` compares double options: NaN is
  never equal (even to NaN), signed zeros `+0.0 == -0.0` are equal, same
  infinities are equal, and finite values compare via 64-bit IEEE representation.
- In `brisque_math.h`, `brisque_range_scale` asserts
   `span != 0.0 && isfinite(span)` on `span = hi - lo` instead of raw
   `hi != lo`.

Do not revert these helpers or assertions to raw `==` or `!=`.
