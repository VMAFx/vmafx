<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-0999: Guard `<stdatomic.h>` includes in C++ translation units (GCC 14 + Clang-18 fix)

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `build`, `ci`, `cpp`, `atomics`, `tsan`, `fork-local`

## Context

The TSan CI job (`sanitizers.yml`) compiles `core/` with Clang-18 as the C++ compiler
against GCC 14 system headers on Ubuntu 24.04.  `feature_extractor.cpp` failed to
compile with 12 errors of the form:

```text
typedef redefinition with different types ('_Atomic(int)' vs 'atomic<int>')
```

The include chain causing the conflict was:

1. `feature_extractor.h` (correctly guards C++: includes `<atomic>`, `using std::atomic_int`)
2. `feature_extractor.h` then includes `framesync.h`
3. `framesync.h` unconditionally includes `<stdatomic.h>`
4. GCC 14's `stdatomic.h` C++ wrapper re-includes `<atomic>`, exposing `atomic<int>` as `atomic_int`
5. Clang-18's own `stdatomic.h` then fires and tries to `typedef _Atomic(int) atomic_int` —
   a conflict with the already-declared `atomic<int>` typedef

`framesync.h` declares no `atomic_*` types; the `<stdatomic.h>` include was vestigial.

`ref.h` had a related narrower guard (`defined(_MSC_VER)` only) that would hit the same
conflict whenever `ref.h` is included from a non-MSVC C++ translation unit on GCC 14 +
Clang-18; the assumption in the prior comment that "gcc/clang on Linux/macOS surface both
as a GNU extension" no longer held with GCC 14.

## Decision

Add a `#if !defined(__cplusplus)` guard around the `<stdatomic.h>` include in `framesync.h`
(the include is vestigial there — no atomic types are used in the header's declarations).

Widen the `ref.h` guard from `defined(__cplusplus) && defined(_MSC_VER)` to
`defined(__cplusplus)` so all C++ compilers use the `<atomic>` + `using std::atomic_int`
path, not just MSVC.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add `-include <atomic>` to C++ compile flags | Fixes all TUs at once | Imposes a hidden include on every C++ file; harder to audit | Header-level fix is self-documenting and localised |
| Add `#undef atomic_int` after the include | Avoids changing the include logic | Undefined-macro tricks are fragile; violates Power-of-10 rule 1 (no macro abuse) | Not chosen |
| Guard only `framesync.h` | Minimal change, fixes the failing TU | `ref.h` retains the narrower MSVC-only guard and would break under the same GCC 14 + Clang-18 toolchain if `ref.cpp` is ever added to the meson build | Wider fix is low-risk and correct |

## Consequences

- **Positive**: TSan build (`-Db_sanitize=thread -Denable_cuda=false -Denable_sycl=false`)
  compiles `feature_extractor.cpp` cleanly on GCC 14 + Clang-18. The fix is backward-compatible:
  C TUs still use `<stdatomic.h>` unchanged.
- **Negative**: None; the changed headers expose no new symbols and alter no ABI.
- **Neutral / follow-ups**: Any future header that includes `<stdatomic.h>` without a C++
  guard should apply the same pattern (check with `grep '#include <stdatomic.h>'` across `.h` files).

## References

- CI run showing the failure: `gh run view 26914915322 -R VMAFx/vmafx --log-failed`
- GCC 14 `stdatomic.h` wrapper issue with Clang:
  [gcc.gnu.org/bugzilla/show_bug.cgi?id=114007](https://gcc.gnu.org/bugzilla/show_bug.cgi?id=114007)
- `feature_extractor.h` existing C++ guard: lines 25–30 (ADR-0772)
- Related file: `core/src/framesync.h`, `core/src/ref.h`
