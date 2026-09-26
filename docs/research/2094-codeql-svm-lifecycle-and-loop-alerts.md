<!-- markdownlint-disable MD013 -->
# Research-2094: CodeQL SVM solver lifecycle and parser loop alerts

- **Status**: Active
- **Workstream**: [ADR-0889](../adr/0889-libsvm-vendored-audit.md), [ADR-1039](../adr/1039-vendored-svm-realloc-oom-safety.md), [ADR-1142](../adr/1142-whole-codebase-standards.md)
- **Last updated**: 2026-09-24

## Question

Whether GitHub CodeQL alerts 1222–1225 (`cpp/resource-not-released-in-destructor` on
`alpha_status`, `active_set`, `G`, and `G_bar` in `class Solver`) and alert 1226
(`cpp/loop-variable-changed` on the support-vector indexing loop in
`parse_support_vectors()`) in vendored libsvm `core/src/svm.cpp` represent genuine
lifecycle and bounds defects or false positives, whether `Solver` always reaches
`solve_finish()`, whether exception or early-exit paths can leak resources, whether
adding destructor cleanup risks double-freeing or violating the libsvm lifecycle, and
how to eliminate the root causes without suppressing scanner findings or regressing
model outputs.

## Sources

- Upstream libsvm 3.24 source (`core/src/svm.cpp`, `core/src/svm.h`) by Chih-Chung
  Chang and Chih-Jen Lin.
- GitHub CodeQL Security Alerts API: alerts 1222–1226 on `core/src/svm.cpp`.
- [ADR-0889](../adr/0889-libsvm-vendored-audit.md): text-format
  model parser rejection hardening.
- [ADR-1039](../adr/1039-vendored-svm-realloc-oom-safety.md): CERT MEM04-C realloc OOM
  hardening in vendored libsvm.
- [ADR-0141](../adr/0141-touched-file-cleanup-rule.md): touched-file cleanup rule.
- Netflix golden output assertions (`vmaf_v1.0.16_3d0h` and `vmaf_v0.6.1`).

## Findings

### 1. Solver lifecycle analysis (Alerts 1222–1225)

CodeQL flags `alpha_status`, `active_set`, `G`, and `G_bar` as resources allocated
via `new[]` in `Solver::solve_setup()` that are not released in `~Solver()`.

- **Happy-path reachability**: In normal execution without C++ exceptions,
  `Solver::Solve()` contains no early returns; it runs its optimization loop and
  unconditionally calls `solve_finish()`. Prior to this fix, `solve_finish()`
  executed `delete[]` on all seven heap buffers (`p`, `y`, `alpha`, `alpha_status`,
  `active_set`, `G`, `G_bar`).
- **Destructor double-free hazard**: `solve_finish()` previously freed those
  seven pointers without setting them to `nullptr`. If a naive `delete[]` cleanup was
  added to `~Solver()` without nulling, any stack-allocated `Solver` or `Solver_NU`
  instance (such as in `svm_train_one()`) immediately suffered a fatal double-free
  abort upon returning from `Solve()`.
- **Exception leak hazard**: In `solve_setup()`, seven allocations take place in
  sequence (`p`, `y`, `alpha`, `alpha_status`, `active_set`, `G`, `G_bar`). If an
  intermediate allocation throws `std::bad_alloc`, or if virtual method calls throw,
  execution unwinds without reaching `solve_finish()`. Because `~Solver()` was empty,
  all previously allocated buffers leaked permanently.
- **Idempotent RAII cleanup**: Defining `Solver::solve_cleanup()` to delete and
  nullify all seven pointers ensures that cleanup is safe to invoke repeatedly:

  ```cpp
  void Solver::solve_cleanup()
  {
      delete[] p;            p = nullptr;
      delete[] y;            y = nullptr;
      delete[] alpha;        alpha = nullptr;
      delete[] alpha_status; alpha_status = nullptr;
      delete[] active_set;   active_set = nullptr;
      delete[] G;            G = nullptr;
      delete[] G_bar;        G_bar = nullptr;
  }
  ```

  Calling `solve_cleanup()` in `solve_finish()`, in `~Solver()`, and at the very
  entry of `solve_setup()` guarantees:
  1. No leak occurs during exception unwinding.
  2. No double-free occurs when `~Solver()` executes after `solve_finish()`.
  3. No leak occurs if a `Solver` instance is re-used across multiple `solve_setup` calls.
  4. Deleted copy constructor and assignment operators (`Solver(const Solver &) = delete;`)
     prevent accidental shallow copies.

### 2. Parser loop variable mutation (Alert 1226)

CodeQL alert 1226 flagged `parse_support_vectors()`:

```cpp
for (size_t i = 0; i < sv_buffer.size(); ++i) {
    ...
    while (support_vectors[i].index != -1) {
        ++i;
    }
}
```

Modifying `i` inside the body of a `for` loop that already increments `i` violates
rule `cpp/loop-variable-changed` and risks skipping bounds checks or reading past
buffer boundaries if an end-of-vector sentinel (`index == -1`) is missing.

Replacing the construct with a standard `while (i < sv_buffer.size())` loop,
checking `i < sv_buffer.size()` during inner traversal, and asserting
`exceptAssert(i < sv_buffer.size(), "Support-vector run missing sentinel")`
eliminates the alert, prevents out-of-bounds reads, and preserves the strict HISS-04
cap of $\le 60$ LOC for the function (59 LOC).

## Alternatives explored

| Option | Result | Decision |
| --- | --- | --- |
| Suppress or dismiss alerts as false positives | Leaves real exception leak paths unhandled; rejects audit policies | Rejected |
| Migrate raw buffers to `std::vector` | Changes vendored libsvm memory layout, header structures, and hotpath semantics | Rejected |
| Naive `delete[]` in `~Solver()` | Causes reproducible double-free abort in `svm_train_one` | Rejected |
| Idempotent `solve_cleanup()` in `~Solver()` and `solve_finish()` + pointer nulling | Completely eliminates leak and double-free hazards; fully exception safe | **Chosen** |
| Retain `for` loop with inner increments | Leaves CodeQL alert 1226 active and risks out-of-bounds reads | Rejected |
| Bounded `while` loop with sentinel assertion | Eliminates alert 1226, enforces bounds, stays $\le 60$ LOC | **Chosen** |

## Open questions

None. All five alerts (1222–1226) are conclusively resolved by proved root-cause
remediation.

## Related

- ADR-0889: text-format model parser rejection hardening.
- ADR-1039: CERT MEM04-C realloc OOM hardening.
- ADR-0141: touched-file cleanup rule.
- CodeQL security alerts 1222, 1223, 1224, 1225, 1226.
