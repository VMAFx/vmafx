---
paths:
  - core/src/predict.c
  - core/src/interop/pelorus_interop.c
invariant: Piecewise linear mapping rejects non-finite scores; Pelorus interop validates framing and QP bounds.
area: models-and-rust
---
<!-- markdownlint-disable MD013 -->
# Scoring arithmetic and Pelorus interop

`predict.c` and `interop/pelorus_interop.c` hold scoring arithmetic. Helpers
there were cut at statement boundaries only. Never split one arithmetic
expression across helper, and never reorder accumulation: FMA contraction
and re-association both move scores (ADR-1253).

`predict.c::piecewise_linear_mapping` rejects non-finite input before writing
its `0.0` initialization (ADR-1302). Every ordered segment comparison is false
for NaN, so moving that initialization back above guard converts failed
model computation into successful zero prediction. production path also
routes post-denormalization, polynomial and piecewise results through
`predict_validate_finite`; it emits one warning naming frame and value and
returns before collector publication. regressions require caller-owned
output to remain unchanged on `-EINVAL`, no model score in collector, and
exactly one diagnostic rather than one warning per mapping segment.
pure linear, piecewise, and bitwise-equality helpers live in
`predict_internal.h` as `static inline` definitions shared by `predict.c` and
`test_predict.c`; keep their expression text and evaluation order identical.
test links production predictor for end-to-end scoring and checks its
real `vmaf_log` output in `test_predict_nonfinite_log_output.py`. Never restore
old `#include "predict.c"` or `VMAF_PREDICT_TEST_NONFINITE_LOG` override:
that created second static call graph and hid production diagnostic.
Meson owns production TU through `predict_c_lib`: `libvmaf` extracts that
object and private-source test binaries link `predict_c_dependency`. Never put
`predict.c` back in `libvmaf_sources` or test source list. Whole-build CodeQL
coalesces repeated external definitions but retains orphan copy of
private scan graph, and compiling TU 53 times also wastes build capacity.

Two `interop/pelorus_interop.c` invariants that split introduced, both
pinned by ADR-1142 clang-tidy ratchet (file's allowance is 7):

- `blob_validate_framing` publishes `const PelorusSideData *` it already
  derived. `pel_blob_find_section` consumes that pointer instead of casting
  image bytes to header second time, so blob is cast to its header in
  exactly one place per constness. Re-deriving it locally costs one extra
  `bugprone-casting-through-void` and breaks ratchet.
- `qp_cell_average` holds per-cell block fold. It exists so that
  `qp_fold_blocks_to_cells` stays inside `readability-function-size`
  `NestingThreshold` of 4 — inlining it back puts innermost statement at
  level 5. Its `int64_t sum` accumulates row-major over clamped block
  window and division truncates, so any reordering moves cell values
  (ADR-1253).
- `validate_pack_args` takes `out_len` as `const size_t *`: it inspects
  caller's out-parameters for NULL and never writes through them
  (`readability-non-const-parameter`). `pel_blob_pack` still owns both stores.
