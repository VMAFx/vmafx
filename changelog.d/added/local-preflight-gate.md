- **`make preflight`** runs the CI checks a single-compiler local build cannot
  catch: clang and 32-bit builds, ASan/UBSan, clang-tidy on the touched files,
  cppcheck, and a static check for constructs MSVC rejects. The documented local
  gate built with one compiler, so a change could pass locally and fail several
  CI lanes at once — a 32-bit-false `static_assert`, an MSVC-invalid
  `align((N))` and a clang-invalid `__attribute__(x)` all shipped that way on
  one afternoon, costing three round-trips on a queue that allows a single
  active PR. Each now fails locally in seconds. `--list` shows which CI context
  each stage mirrors and `--stage NAME` re-runs one. See
  [ADR-1234](docs/adr/1234-local-preflight-gate.md).
