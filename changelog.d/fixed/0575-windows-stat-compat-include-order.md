- `Build — Windows MinGW64 (CPU)` and `Build — Windows MSVC + CUDA` CI
  legs now compile cleanly against any Windows SDK version (ADR-0575).
  ADR-0521's POSIX-to-MSVC alias macros (`#define stat __stat64`, etc.)
  were placed before `#include <sys/stat.h>`, causing the preprocessor
  to macro-expand identifiers inside system headers: MinGW64 raised
  "redefinition of struct _stat64"; SDK 10.0.26100.0 + NVCC raised
  cascading C2059/C2143. Fix: include `<sys/stat.h>` first (pristine),
  then define the aliases; change the guard from `#ifdef _WIN32` to
  `#ifdef _MSC_VER` so MinGW64 (which ships native POSIX stat) is
  excluded.
