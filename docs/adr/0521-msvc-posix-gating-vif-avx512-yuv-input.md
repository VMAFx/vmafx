<!-- markdownlint-disable MD013 MD060 -->
# ADR-0521: MSVC portability gating — `vif_avx512.c` noinline/noclone + `yuv_input.c` S_ISREG/fstat

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris
- **Tags**: `ci`, `build`, `windows`, `msvc`, `simd`, `tools`, `portability`, `fork-local`, `bugfix`

## Context

After PR #1274 (ADR-0515) fixed the MinGW64 leg by replacing the `/tmp`-hardcoded
`mkstemp` call, the `Build — Windows MSVC + CUDA` and `Build — Windows MSVC + oneAPI
SYCL` matrix legs remained red on master for two distinct reasons:

**MSVC+CUDA failure (`vif_avx512.c`):**
`core/src/feature/x86/vif_avx512.c` used the GCC/Clang extension
`__attribute__((noinline, noclone))` on two private helpers
(`vif_subsample_rd_8_vert_j`, `vif_subsample_rd_8_horiz_j`) introduced by
ADR-0503 to isolate register pressure in the AVX-512 VIF kernel.
MSVC's `cl.exe` does not support `__attribute__` syntax at all — it raises
`C2143: syntax error: missing ')' before '('` and cascades into ~80 downstream
`C2065: undeclared identifier` errors for every parameter in the function body.

**MSVC+SYCL failure (`yuv_input.c`):**
`core/tools/yuv_input.c::yuv_check_file_size` called `fstat()` and
`S_ISREG()` on the return of `fileno(fin)`.  The file already guarded
`<unistd.h>` and `fileno` → `_fileno` correctly, but `S_ISREG` is a POSIX
macro not defined by MSVC's `<sys/stat.h>`.  Intel's oneAPI SYCL compiler
(`icx-cl`) enforces ISO C99 strictness and treats the implicit-function call as
a hard error (`error: call to undeclared function 'S_ISREG'`).  `fstat` itself
is also a POSIX name; the MSVC equivalent is `_fstat64` / `struct __stat64`.
`off_t` is defined but is 32-bit on MSVC by default, which would truncate sizes
above 2 GiB.

Neither issue surfaced before PR #1274 because:

- The `vif_avx512.c` helper functions were introduced in PR #1261 (ADR-0503,
  merged just prior).
- The `yuv_input.c` `S_ISREG` check was added post-PR #1274 or was previously
  masked by the CUDA build stopping earlier on a different error.

MSVC builds are "build only" (no test run) in the CI matrix; the Windows MSVC
legs exist to prove the header and source surface compiles clean under cl.exe
and icx-cl, catching portability regressions before they affect downstream
integrators who target Windows natively.

## Decision

**`vif_avx512.c`:** Define a `VMAF_NOINLINE_NOCLONE` portability macro at the
top of the TU, immediately after the `#define MAX` line:

```c
#if defined(_MSC_VER)
#define VMAF_NOINLINE_NOCLONE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define VMAF_NOINLINE_NOCLONE __attribute__((noinline, noclone))
#else
#define VMAF_NOINLINE_NOCLONE
#endif
```

Replace both `static __attribute__((noinline, noclone)) void` declarations with
`static VMAF_NOINLINE_NOCLONE void`.  The `noclone` semantic is meaningful only
on GCC (prevents constant-propagated clone synthesis); MSVC does not clone, so
`__declspec(noinline)` is sufficient there.

**`yuv_input.c`:** Extend the existing `#ifdef _WIN32` block to define POSIX
aliases for `fstat`, `struct stat`, `S_ISREG`, and `off_t` that map to their
MSVC equivalents:

```c
#define fstat(fd, st)  _fstat64((fd), (st))
#define stat           __stat64
#define S_ISREG(m)     (((m) & _S_IFMT) == _S_IFREG)
typedef __int64 off_t;
```

This keeps the function body of `yuv_check_file_size()` unguarded and portable,
and ensures file-size arithmetic is 64-bit on MSVC.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Wrap `vif_avx512.c` helpers in `#ifndef _MSC_VER` no-ops | No macro footprint | Leaves the noinline hint absent on all non-GCC/Clang compilers; alters the register-pressure isolation that ADR-0503 depends on | Drops the benefit on any future cl.exe-based SIMD path |
| Wrap entire `yuv_check_file_size` body in `#ifndef _WIN32` | Minimal LOC change | Silently disables the file-size safety check on Windows builds | The check is exactly the kind of misuse-guard that matters on all platforms |
| Use `#pragma comment(lib, ...)` + `_fstat64` inline without a macro | Slightly more explicit | Pollutes the function body with `#ifdef _WIN32` guards around every POSIX name | Makes future readers untangle three interleaved branches instead of one |

## Consequences

- **Positive**: Both MSVC+CUDA and MSVC+SYCL CI legs compile clean. The
  register-pressure isolation from ADR-0503 is preserved on GCC/Clang; MSVC
  gets `__declspec(noinline)` which is sufficient to prevent inlining. File-size
  checking in `yuv_check_file_size` works correctly on MSVC with 64-bit
  arithmetic.
- **Negative**: None material. The `VMAF_NOINLINE_NOCLONE` macro is
  TU-scoped (not public), so it does not pollute the public header surface.
- **Neutral**: The `#define stat __stat64` alias in the `_WIN32` block must not
  be included in TUs that use `stat()` for directory-entry testing (struct layout
  differs from `struct stat64` on GNU), but `yuv_input.c` only calls `_fstat64`
  — no `lstat` or directory paths.

## References

- [ADR-0503](0503-vif-spill-reduction.md) — introduced the noinline/noclone helpers
- [ADR-0515](0515-test-public-api-score-mingw64-temp-path.md) — sibling MinGW64 fix (PR #1274)
- CI run 26025892576 — PR #1274 MSVC+CUDA job log showing `vif_avx512.c(1045): error C2143`
- CI run 26025892576 — PR #1274 MSVC+SYCL job log showing `yuv_input.c(70,46): error: call to undeclared function 'S_ISREG'`
- MSVC `_fstat64` / `struct __stat64`: <https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fstat-fstat32-fstat64-fstati64-fstat32i64-fstat64i32>
- req: "Fix the Windows MSVC + CUDA / oneAPI SYCL build failures — same file PR #1274 already touched but a different include-portability issue remained."
