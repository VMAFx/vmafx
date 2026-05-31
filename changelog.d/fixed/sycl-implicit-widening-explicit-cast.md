- `core/src/feature/sycl/integer_adm_sycl.cpp`,
  `core/src/feature/sycl/integer_vif_sycl.cpp`:
  Eliminated 12 `bugprone-implicit-widening-of-multiplication-result`
  NOLINTs by replacing implicit widening with explicit `(ptrdiff_t)` casts
  on the leading operand of each stride/accumulator-size multiplication.
  No behavioural change — the operands are bounded by frame dimensions and
  the widening was always intended. PR #127 finding.
