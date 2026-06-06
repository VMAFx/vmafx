### Fixed

- `core/src/compat/win32/pthread.h`: remove duplicate `pthread_once_t` typedef,
  `PTHREAD_ONCE_INIT` macro, and `pthread_once()` definition that caused
  `error: redefinition of pthread_once` on Windows MSVC + CUDA and
  Windows MSVC + oneAPI SYCL builds.
