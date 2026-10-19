- **The Windows MSVC builds compile again.** `vmaf_next_fex_capacity()`
  (`core/src/fex_ctx_vector_internal.h`, #2724) called `std::min()` in a
  translation unit that had read `<windows.h>`, which defines `min` as a
  macro; every MSVC build of `master` stopped with C2589. The call is spelled
  `(std::min)(a, b)`, and the `msvcism` stage of `scripts/dev/preflight.sh`
  now refuses a `min(` / `max(` call behind `<windows.h>` without `NOMINMAX`
  (`scripts/dev/find-windows-minmax.py`).
