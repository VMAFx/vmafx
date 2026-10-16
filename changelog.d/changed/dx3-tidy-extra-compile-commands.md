- **Extra compilation-database entries bring three hand-built sources into clang-tidy coverage.**
  `scripts/ci/gen-mex-compile-commands.py` now provides entries for the LD_PRELOAD fault injector
  and the upstream parity harness, and `scripts/ci/gen-gpu-compile-commands.py` adds the
  standalone HIP dropped-dispatch probe for the HIP lane, retiring their clang-tidy coverage
  exceptions.
