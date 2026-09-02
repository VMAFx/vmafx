<!-- markdownlint-disable MD013 MD036 MD037 MD060 -->
# ADR-0575: Fix yuv_input.c stat compat — include-order and _MSC_VER guard

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris
- **Tags**: `ci`, `build`, `windows`, `msvc`, `mingw`, `tools`, `portability`, `bugfix`, `fork-local`

## Context

ADR-0521 introduced POSIX-to-MSVC alias macros in `core/tools/yuv_input.c`
under `#ifdef _WIN32` to allow `yuv_check_file_size()` to compile under
`cl.exe` and `icx-cl`:

```c
#define fstat(fd, st) _fstat64((fd), (st))
#define stat __stat64
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
typedef __int64 off_t;
```

These macros were placed **before** `#include <sys/stat.h>` (line 41),
which introduced two independent regressions once CI runners rolled to
newer environments:

**Regression 1 — MinGW64 (MSYS2, `windows-latest`)**

MinGW64 defines `_WIN32` (as all Windows toolchains do), but its
`_mingw_stat64.h` already provides a POSIX-compatible `struct _stat64` and
a `_fstat64` declaration. When `#define stat __stat64` is active during
`#include <sys/stat.h>`, the preprocessor expands `_stat64` tokens inside
the system header, producing:

```text
error: redefinition of 'struct _stat64'
error: conflicting types for '_stat64'
error: passing argument 2 of '_fstat64' from incompatible pointer type
```

This broke the `Build — Windows MinGW64 (CPU)` CI leg (CI run 26053401593,
step "Build vmaf", `tools/vmaf.exe.p/yuv_input.c.obj`).

**Regression 2 — MSVC + CUDA (Windows SDK 10.0.26100.0, Win 11 24H2)**

GitHub Actions' `windows-latest` runner image recently defaulted to
Windows SDK 10.0.26100.0. Under this SDK, `ucrt/sys/stat.h` declares
`_fstat64` and `struct _stat64 / __stat64` with visibility attributes that
are sensitive to macro-expansion order. Placing `#define stat __stat64`
before the include caused NVCC and `cl.exe` to expand identifiers inside
the header itself, triggering `C2059` (syntax error) and `C2143` (missing
token) cascades in the `Build — Windows MSVC + CUDA` CI leg.

Neither regression appeared when the original ADR-0521 fix was written
because the MinGW64 headers in the then-current `msys2/setup-msys2`
release handled the macro differently, and the MSVC runner was still using
SDK 10.0.22621.0.

## Decision

Two changes to `core/tools/yuv_input.c`:

1. **Move `#include <sys/stat.h>` before the macro block** so the system
   header parses in its pristine form, without any `stat`-aliasing macros
   active. The macros are then defined in user code after the header has
   already been absorbed.

2. **Change the macro guard from `#ifdef _WIN32` to `#ifdef _MSC_VER`**.
   `_WIN32` is defined by every Windows toolchain (MSVC, MinGW, Clang-cl,
   icx-cl). `_MSC_VER` is defined only by MSVC's `cl.exe` and Intel's
   MSVC-ABI-mode `icx-cl`; MinGW never sets it. MinGW already provides
   POSIX-compatible `stat`, `fstat`, and `S_ISREG` natively, so no aliases
   are needed there.

The `_WIN32` guard for `#include <io.h>` and `#define yuv_fileno _fileno`
is kept as-is; `<io.h>` (which provides `_fileno`) is a genuine
MinGW-compatible header shared by all Windows toolchains.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Pin `windows-latest` runner image or SDK to 22621 in CI YAML | No source change | Pins CI to an old SDK indefinitely; breaks when the pin expires; does not fix the MinGW64 regression | The root cause is macro-before-include ordering and overly broad guard, not the SDK version |
| Add `__MINGW32__` exclusion guard around the macros | Narrower change | `#if defined(_WIN32) && !defined(__MINGW32__)` is semantically `#ifdef _MSC_VER` with extra noise | `_MSC_VER` is the direct and self-documenting form |
| Wrap entire `yuv_check_file_size` body in `#ifndef __MINGW32__` | Sidesteps the include-order issue | Disables the file-size safety check on MinGW64 builds | The check is portable; MinGW supports `fstat` natively |
| Use `#include_next` to defer the header past the macros | Technically correct | MSVC does not support `#include_next` | Gratuitous portability complexity for a simple ordering fix |

## Consequences

- **Positive**: All three Windows CI legs (`Build — Windows MinGW64 (CPU)`,
  `Build — Windows MSVC + CUDA`, `Build — Windows MSVC + oneAPI SYCL`)
  compile cleanly against any SDK version. The `yuv_check_file_size()`
  function body remains unguarded and identical across all platforms.
- **Negative**: None. The change is mechanical; no logic is altered.
- **Neutral**: `#ifdef _MSC_VER` is now the canonical guard for MSVC-only
  compat shims in this file. MinGW-specific workarounds should use
  `#ifdef __MINGW32__` if ever needed separately.

## References

- [ADR-0521](0521-msvc-posix-gating-vif-avx512-yuv-input.md) — original MSVC stat compat fix whose side-effects this ADR resolves
- [ADR-0515](0515-test-public-api-score-mingw64-temp-path.md) — prior MinGW64 portability fix (mkstemp temp-path)
- CI run 26053401593 — MinGW64 failure: `yuv_input.c:33: error: redefinition of 'struct _stat64'` (step "Build vmaf")
- CI run 26053401593 — MSVC+CUDA failure in step "Build libvmaf (CUDA)" (SDK 10.0.26100.0, INCLUDE path confirms)
- Commit `b4b3787dedc018c30e9b6a3a6e334611a1f1507c` — master HEAD at time of failure
- MSVC predefined macros: <https://learn.microsoft.com/en-us/cpp/preprocessor/predefined-macros>
- req: "Fix Windows MSVC+CUDA and MinGW64 CI failures caused by the ADR-0521 stat compat macros being placed before the sys/stat.h include and guarded too broadly with _WIN32."
