---
paths:
  - core/src/svm.cpp
  - core/src/svm.h
invariant: Vendored libsvm preserves thread-locale isolation, JSON in-memory parser, malloc checks, and RAII Solver.
area: models-and-rust
---
<!-- markdownlint-disable MD013 -->
# Vendored libsvm patches, test files, and memory hardening

## 11. Vendored libsvm + IQA test files are observation-only (ADR-0952)

`core/test/test_svm_api.c`, `core/test/test_svm_multiclass.c`, and
`core/test/test_iqa_helpers.c` were added to lift coverage of vendored
bodies (`core/src/svm.cpp`, `core/src/feature/iqa/*.c`) without modifying
any vendored source. Invariant symmetric to ADR-0889 cordon:

`test_svm_multiclass.c` specifically exercises sequential-realloc
double-free path fixed in PR #708 — 17-class and 32-class C_SVC fixtures
force `max_nr_class=16→32` realloc doubling in `svm_group_classes()`,
and 17-class NU_SVC fixture triggers same path in
`svm_check_parameter()`. Under ASan/UBSan any regression to double-free
pattern aborts immediately. (ADR-1066)

- These test files **must not** import any private vendored header,
  call any static-internal helper, or rely on any vendored macro
  beyond public surface declared in `svm.h` / `convolve.h` /
  `decimate.h` / `math_utils.h` / `ssim_tools.h`.
- Vendored cordon `NOLINTBEGIN/NOLINTEND` in `svm.cpp` and
  `tdistler.com` copyright headers in IQA helpers stay
  byte-identical across upstream re-pins.
- `_round()` / `_cmp_float()` asymmetry tests in
  `test_iqa_helpers.c` double as behavioural documentation. They lock
  asymmetric "trunc toward zero, add sign when |frac| >= 0.5"
  rounding rule. If future upstream sync rewrites helper to
  IEEE-754 round-half-to-even, test fails *by design* — failure
  surfaces unintended numerical change at rebase diff, not at
  integration-level SSIM/VMAF anomaly.

When porting upstream Netflix/vmaf commit modifying
vendored libsvm or IQA bodies, test files do not need to follow
upstream change; they observe public-API contracts that survive
across versions. Test failure post-port is signal — investigate
API drift before relaxing assertion.

## 10. Vendored libsvm — five fork patches must not regress on sync (ADR-0889, Research-2094)

`core/src/svm.cpp` + `core/src/svm.h` = verbatim vendored copy of
upstream libsvm 3.24 (Chih-Chung Chang / Chih-Jen Lin), wrapped in
file-level `NOLINTBEGIN` / `NOLINTEND` cordon so fork's
touched-file lint-clean rule does not re-flow vendored body. Five
fork-local patch families live inside that cordon and must survive any
future upstream sync:

1. **Thread-locale isolation** — both `SVMModelParserFileSource` and
   `SVMModelParserBufferSource` constructors call
   `buffer.imbue(std::locale::classic())`. Removing this re-introduces
   ADR-0137's locale-perturbation hazard on hosts whose `LC_NUMERIC`
   uses `,` as decimal separator. *Cite ADR-0137 in any commit
   touching these lines.*
2. **JSON in-memory entry point** — `svm_parse_model_from_buffer` (and
   `SVMModelParserBufferSource` class that backs it) is fork-added;
   upstream libsvm has only `svm_load_model(const char *path)`. Fork's
   `read_json_model.c` depends on buffer entry point.
   Removing it breaks JSON-embedded model loading.
3. **SAN-MODEL-MALLOC-OOB hardening** — every `Malloc(...)` call in
   `parse_header()` and `parse_support_vectors()` whose size depends on
   `nr_class` or `total_sv` is gated by `exceptAssert(... > 0 && ... <=
   VMAF_SVM_MAX_AXIS_COUNT, ...)`. The bound `VMAF_SVM_MAX_AXIS_COUNT
   (1 << 24)` is fork-defined. The `sv_buffer.empty()` post-parse guard
   is fork-added. `model->nr_class > 0` row-ordering precondition
   on `rho`, `label`, `probA`, `probB`, `nr_sv` is fork-added.
   Regression coverage lives in `core/test/test_svm_parser.c` (suite
   `fast`). *Cite sanitizer-real-bug-fixes changelog and ADR-0889
   in any commit touching these guards.*
4. **Solver RAII lifecycle and loop safety (Research-2094)** — `Solver`
   and `Solver_NU` manage working heap arrays (`p`, `y`, `alpha`,
   `alpha_status`, `active_set`, `G`, `G_bar`) via idempotent
   `solve_cleanup()` invoked by `solve_finish()`, `~Solver()`, and entry
   of `solve_setup()`. Deleted copy/assignment operations prevent shallow
   copying and double-free. In `parse_support_vectors()`, support-vector
   parsing replaces outer `for` loop counter mutation with bounded `while`
   loop verifying sentinel termination. Eliminates CodeQL alerts 1222–1226.
5. **`using std::swap;` replaces libsvm's global `swap` template** —
   template + `std::vector<svm_node>` = ambiguous call inside libc++ 23
   `__split_buffer::__swap_layouts` (`using std::swap; swap(...)`, ADL
   on `svm_node *`); file stops compiling. Never restore template.
   libsvm `min` / `max` stay deliberately: tie or NaN operand returns
   second argument, `std::min` / `std::max` return first — switching
   changes training (zero-rho sign, NaN propagation). Upstream fix for
   issue 1616 takes `std::min` / `std::max` too; keep libsvm pair on
   sync unless that semantic change gets separate decision.
   `Solver_NU` overrides carry `override` (clang
   `-Winconsistent-missing-override`).

Additionally:

- `model->free_sv = 1;` at end of `parse_support_vectors` is
  load-bearing invariant for `svm_free_and_destroy_model`'s ownership
  transfer. Vendor-original; never flip it.
- `LIBSVM_VERSION 324` in `svm.h` = pin. sync to newer
  version must re-apply three patch families above and re-run
  `core/test/test_svm_parser.c` + `core/test/test_predict` +
  `core/test/test_model`. See ADR-0889 for deferral rationale on
  upstream 3.36 sync.
