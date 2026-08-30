- Fix TSan CI build break on GCC 14 + Clang-18: guard `<stdatomic.h>`
  in `framesync.h` (vestigial include) and widen `ref.h` C++ guard from
  MSVC-only to all C++ compilers, resolving a `typedef redefinition` error
  in `feature_extractor.cpp` caused by GCC 14's C++ `stdatomic.h` wrapper
  + Clang-18's own `stdatomic.h` re-firing in the same TU. (ADR-0999)
