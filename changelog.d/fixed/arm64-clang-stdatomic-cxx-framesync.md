- **`Build — Ubuntu ARM clang (CPU)` CI failure: typedef redefinition
  `atomic_int` (`_Atomic(int)` vs `atomic<int>`) on aarch64 clang-18 +
  GCC-14 headers.** `framesync.h` included `<stdatomic.h>` unconditionally.
  In C++ mode, GCC-14's `<atomic>` already defines `atomic_int` as
  `atomic<int>` (via its internal `<stdatomic.h>` wrapper); clang-18's
  independent `<stdatomic.h>` then attempted to redefine it as
  `_Atomic(int)`, producing 12 errors in `feature_extractor.cpp`.
  Fix: guard the `<stdatomic.h>` include in `framesync.h` with
  `#if !defined(__cplusplus)` — the header's public interface exposes
  no atomic types (the `VmafFrameSyncContext` struct is opaque); C TUs
  that include `framesync.h` directly still receive `<stdatomic.h>` via
  the unguarded C path.
