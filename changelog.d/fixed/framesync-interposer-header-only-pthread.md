- `test_framesync_init_failure` now injects its pthread failures through a
  force-included header (`core/test/test_framesync_interpose.h`) instead of
  four `-D` flags on the compile line. A command-line `-D` is already in force
  when `<pthread.h>` is read, so it renamed the entry points that header
  *declares* as well as framesync.c's calls to them. That is harmless where
  the declarations are `extern` (glibc, macOS libSystem, MinGW winpthreads)
  and fatal where `<pthread.h>` defines them inline: `core/src/compat/win32/`
  `pthread.h`, the SRWLOCK / CONDITION_VARIABLE shim every MSVC and clang-cl
  build uses, is header-only and `static inline`, so the rename landed on the
  definitions, framesync.c called the real primitives under the wrapper's
  name, and no injected failure ever occurred. The header reads `<pthread.h>`
  first and defines the renames after it, so they reach call sites only.
  Surfaced on `Windows ARM64 MSVC`, the one MSVC lane that runs `meson test`
  rather than a named subset of test executables.
