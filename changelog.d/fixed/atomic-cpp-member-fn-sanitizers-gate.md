- `feature_extractor.cpp` (C++ TU, ADR-0772): replace C11 free-function
  `atomic_load()` / `atomic_init()` / `atomic_fetch_add()` / `atomic_fetch_sub()`
  calls with `std::atomic<int>` member-function equivalents (`.load()`, `.store()`,
  `.fetch_add()`, `.fetch_sub()`). Clang 18 (used in the Sanitizers CI build)
  rejects `__c11_atomic_*` builtins when the argument is `std::atomic<int> *`
  rather than a `_Atomic`-qualified type; GCC tolerated the mismatch silently.
  This was a build failure in the Sanitizers gate (TSan job) introduced by the
  C→C++ rename. The C twin (`feature_extractor.c`) retains the C11 free functions,
  which remain correct in a C translation unit.
