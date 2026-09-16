- **SYCL lint, third pass, and a correction to the counts.**
  `modernize-use-nullptr` is now clean in the SYCL C++ translation units.
  ADR-1138 exempts only *C* TUs — where MSVC's `/std:clatest` has no C23
  `nullptr` — so the `.cpp` kernels get the keyword rather than a
  suppression band.
  **The warning counts reported in the two preceding entries were wrong.**
  They were measured while `ninja` had regenerated
  `build-sycl/compile_commands.json` and dropped the entries
  `scripts/ci/gen-sycl-compile-commands.py` synthesises for the SYCL TUs,
  so clang-tidy silently fell back to default flags and under-counted.
  Re-measured with the database regenerated immediately before each run,
  the SYCL lane went **517 → 345 → 303**. The code changes were never in
  doubt — each was verified by building with `icpx` and running 195/195
  on the Arc A380 — only the numbers were. The lesson is the one the
  repo already learned once: a tool that silently degrades when its
  input is stale will report progress you did not make, so regenerate
  the database and check the parse-error count before trusting a delta.
