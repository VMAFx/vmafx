### Fixed

- `core/src/feature/x86/vif_avx512.c`: remove dead `ALIGNED(x)` macro block
  defined inside `vif_statistic_8_avx512()` but never referenced within the
  TU. The block was a copy-paste artefact from `vif_avx2.c` that also carried
  the obsolete `!defined UNDER_CE` Windows CE guard.
- `core/src/feature/x86/vif_avx2.c`: drop the dead `&& (!defined UNDER_CE)`
  condition from the `ALIGNED(x)` macro definition. Windows CE is not a
  supported target on any fork build matrix; the condition made the macro
  expand to a no-op on a hypothetical CE+MSVC host instead of to the correct
  `__declspec(align(x))`.  Simplified to a plain `#elif defined(_MSC_VER)`.
- `core/src/dnn/model_loader.c`: move the `extern char **environ` declaration
  inside the existing `#ifndef _WIN32` block so the POSIX-only external
  reference is only visible in builds that actually use it.  On MSVC the
  declaration was harmless (MSVC CRT exports `environ` under that name) but
  created unnecessary cross-platform ambiguity.
