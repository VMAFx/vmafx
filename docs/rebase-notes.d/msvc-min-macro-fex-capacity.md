## `min()` / `max()` behind `<windows.h>` in C++ translation units (2026-10-11)

`fix/msvc-min-macro-fex-capacity`, under
[ADR-1234](adr/1234-local-preflight-gate.md) (local preflight gate).

- `core/src/fex_ctx_vector_internal.h`: `vmaf_next_fex_capacity()` spells its
  bound `(std::min)(count_limit, bytes_limit)`. Fork-only file; nothing to
  take from upstream. **On sync or rebase**: do not write the call back as
  `std::min(...)`; on Windows `fex_ctx_vector.cpp` has read `<windows.h>`
  through `core/src/compat/win32/pthread.h` before this header, and MSVC
  stops with C2589.
- `core/src/compat/win32/pthread.h` includes `<windows.h>` without
  `NOMINMAX`. Until that changes, every C++ translation unit that includes
  `<pthread.h>` on Windows has the `min` and `max` macros: a C++ port from
  upstream that calls `std::min()`, `std::max()` or
  `std::numeric_limits<T>::max()` after it needs the parenthesised spelling.
- `scripts/dev/preflight.sh`: the `msvcism` stage runs
  `scripts/dev/find-windows-minmax.py` on the changed sources and fails on a
  finding or on a scan that did not run.
- Reproducer: `bash scripts/ci/tests/test-preflight-msvcism.sh`, and
  `scripts/dev/preflight.sh --full --stage msvcism` on the tree.
