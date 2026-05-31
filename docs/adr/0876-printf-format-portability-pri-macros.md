<!-- markdownlint-disable MD060 -->
# ADR-0876: Adopt `<inttypes.h>` PRI macros for fixed-width integer printf formatting

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris
- **Tags**: portability, c-standards, cert, sycl, dnn, windows

## Context

The fork's C / C++ sources sometimes formatted fixed-width integer types
(`uint64_t`, `int64_t`) using literal length-modifier specifiers such as
`%lu`, `%lld`, or `%llx`, paired with an explicit cast (`(unsigned long)`,
`(long long)`, `(unsigned long long)`).

That pattern is *almost* portable on Linux LP64 — `unsigned long ==
uint64_t` there — but it silently truncates on Windows LLP64, where
`unsigned long` is 32 bits and `uint64_t` is 64 bits. The cast happens
without a diagnostic; the 32-bit-truncated value is what reaches the
log line. CERT FIO47-C explicitly calls this out as a portability bug
class, and MISRA C:2012 Rule 21.6 warns against this kind of
length-modifier mismatch.

The pre-existing `(long long)` + `%lld` cast on `int64_t` is technically
portable (C99 guarantees `long long >= 64 bits` and `%lld` matches it)
but is brittle: future readers can't tell whether the cast was load-bearing
or accidental. The `<inttypes.h>` PRI macros (`PRId64`, `PRIu64`,
`PRIx64`, `PRIu32`, …) are the standard-mandated portable form and make
the intent explicit.

This audit covers fork-added and post-ADR-0700 fork-modified C / C++
sources under `core/src/`, `core/tools/`, `core/test/`. It does **not**
modify upstream-mirror code (e.g. the `print_128_64` debug macro in
`core/src/feature/x86/adm_avx512.c`) — that's reserved for an upstream
sync.

Three call sites in scope are not bugs and were intentionally left
alone:

1. `core/tools/yuv_input.c` — `(long long)file_sz` on `off_t`. `off_t`
   is a POSIX non-fixed-width type; CERT recommends the `(long long)` +
   `%lld` cast (or `(intmax_t)` + `%jd`). Either form is portable.
2. `core/test/test_public_api_score.c` — `(unsigned long)GetCurrentProcessId()`
   on Windows `DWORD`. `DWORD` is exactly `unsigned long` on Windows;
   `%lu` matches.
3. `core/src/feature/x86/adm_avx512.c` — upstream Netflix debug macro;
   no fork edit.

## Decision

We will use the `<inttypes.h>` PRI macros (`PRId64`, `PRIu64`, `PRIx64`,
`PRIu32`, …) for every printf-family format string in fork-added code
that takes a fixed-width integer type (`int64_t`, `uint64_t`, `int32_t`,
`uint32_t`). The matching include is `<inttypes.h>` for C or
`<cinttypes>` for C++.

Non-fixed-width POSIX types (`off_t`, `pid_t`, `time_t`, `ssize_t`,
`size_t` other than `%zu`) keep their existing explicit-cast +
matching-length-modifier idiom, since no PRI macro applies.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `<inttypes.h>` PRI macros (chosen) | Standard-mandated; portable across LP64 / LLP64 / 32-bit; intent is explicit; readable in lint output | Slight visual overhead at format-string sites (`"%" PRIu64`) | The bar for fork-added portability code is C99 conformance + Windows MinGW build cleanliness — PRI macros are the only spec-mandated way to satisfy both. |
| Keep `(unsigned long long)` + `%llu` cast pattern | Already in use; works on Linux LP64 and Windows LLP64 (because `unsigned long long >= 64 bits`) | Doesn't express intent for fixed-width types; future readers can't tell if cast is load-bearing; silent truncation if anyone forgets the cast | The `(unsigned long)` + `%lu` variant of the same idiom *does* truncate on Windows LLP64; standardizing on PRI macros prevents the whole class. |
| `(intmax_t)` + `%jd` everywhere | One idiom for every integer type | `intmax_t` is wider than necessary; loses the type information; less idiomatic | Adds zero portability over PRI macros and reads worse. |

## Consequences

- **Positive**: Eliminates the Windows-LLP64 truncation bug class for
  fixed-width types in fork-added code. Makes future Windows MinGW
  builds (signalled by memory `project_windows_ci_sdk_pin`) safer
  without warnings. CERT FIO47-C / MISRA 21.6 compliant.
- **Negative**: Slight visual overhead at format-string sites. Adds
  one `#include <inttypes.h>` (or `<cinttypes>`) to four touched
  files.
- **Neutral / follow-ups**: Subsequent fork-added code should follow
  the same convention. A future upstream sync may bring back the
  Netflix `%lld` debug macro in `adm_avx512.c`; that one stays as-is
  (it's behind an `#if 0` and matches `_mm_extract_epi64`'s
  documented return type of `long long`).

## References

- CERT FIO47-C: "Use valid format strings".
- MISRA C:2012 Rule 21.6: "The Standard Library input/output functions
  shall not be used".
- C99 §7.8.1: `<inttypes.h>` PRI macros.
- Memory: `project_windows_ci_sdk_pin` — Windows CI rolled to 24H2 SDK;
  the audit is forward-looking.
- Related PRs: this PR.
- Source: caller-supplied task brief
  ("Audit printf-family format specifiers for portability bugs (CERT
  FIO47-C / MISRA)").
