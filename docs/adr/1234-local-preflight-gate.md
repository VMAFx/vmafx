<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1234: The local gate builds with every compiler CI does

- **Status**: Accepted
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: ci, build, agents

## Context

The documented local gate — `make lint`, `meson test -C build` — builds with
**one** compiler. CI builds with several: `Ubuntu gcc+DNN`, `Ubuntu clang+DNN`,
`Ubuntu i686 gcc`, `Windows MSVC+CUDA`, `Windows MSVC+SYCL`, `Windows MinGW64`,
and four Sanitizer lanes. A change can be green locally and red in three of
those at once.

That is not hypothetical. On 2026-09-07 one branch shipped three separate
portability breaks in a single afternoon, each green under local gcc:

| Break | Locally | CI |
| --- | --- | --- |
| `static_assert(UINT_MAX <= SIZE_MAX/2/sizeof(ptr))` | passes on LP64 | `Ubuntu i686 gcc` cannot compile the file — false on 32-bit |
| `#define ALIGNED(x) __declspec(align((x)))` | gcc never sees the MSVC branch | `Windows MSVC+CUDA` C2059 on every use — MSVC needs a literal |
| `__attribute__(noinline)` (one paren lost) | gcc accepts it | `Ubuntu clang`, `clang+DNN` and all four Sanitizer lanes fail to compile |
| `nullptr` in a C translation unit | gcc and clang accept the C23 keyword | `Windows MSVC+CUDA` C2065 — MSVC's `/std:clatest` does not implement it (ADR-1138) |
| `static const double` initialising a `static` aggregate | silent under `-std=c23 -pedantic-errors -Weverything` | `Windows MSVC+CUDA` C2099, then a cascade of C2440s as the remaining initialisers shift |
| `M_PI` without the `_USE_MATH_DEFINES` / `#ifndef` two-step | glibc exposes it via meson's `-D_GNU_SOURCE` | `Windows MinGW64` `'M_PI' undeclared` — MinGW ignores `_GNU_SOURCE`, and `-std=c23` defines `__STRICT_ANSI__` |
| A `.metal` local named `half` | fine in the C / CUDA / HIP twins the kernel was ported from | `macOS Clang+Metal` redeclaration error — `half` is a built-in MSL type, and every later use then parses as a type name |

The last two rows arrived the same evening in PR #1340, both in one new test
file, and are the reason the `msvcism` stage grew a scanner alongside its
grep patterns: neither is expressible as a line-local regex, and no locally
available compiler diagnoses the second one at any warning level.

Each cost a full CI round-trip on a queue where **one PR at a time** may be in
flight (the strict single-active-PR rule). Three round-trips is roughly ninety
minutes of queue time, and the branch held the merge window for about three
hours in total. Every one of the three was catchable in seconds on the
developer machine.

The common cause was also identifiable: each came from generalising a fix by
regex across a file rather than editing only the sites the checker named. A
local gate that compiles what CI compiles turns that class of mistake from a
CI round-trip into an immediate local failure.

## Decision

Add `scripts/dev/preflight.sh`, run as `make preflight`, which reproduces the
CI lanes that are reproducible on Linux and statically approximates the one
that is not:

| Stage | Mirrors | Catches |
| --- | --- | --- |
| `gcc` | Ubuntu gcc(+DNN) | the baseline build |
| `clang` | Ubuntu clang(+DNN) | clang-only syntax, e.g. `__attribute__(x)` |
| `m32` | Ubuntu i686 gcc | 32-bit-only `static_assert` / width assumptions |
| `msvcism` | Windows MSVC+CUDA / +SYCL | constructs MSVC rejects, without needing MSVC |
| `sanitizers` | Sanitizers (address/undefined) | UB the plain build hides |
| `tidy` | Tidy Changed | clang-tidy on the touched files, with the workflow's exclusion list |
| `cppcheck` | Cppcheck | cppcheck's findings |

Two properties are deliberate. It considers **uncommitted** work, not just
`origin/master...HEAD` — a preflight that ignored the edit you are about to
commit would miss the point (the first version did exactly that and passed
against a planted break). And a missing toolchain **skips** its stage with a
notice instead of failing, so the script stays useful on a partially
provisioned machine.

The `m32` stage compiles with `-fsyntax-only` and `-I build/src`, because a
full 32-bit build needs multilib runtime libraries while the failure mode it
exists for — an assumption about the width of `size_t` / `ptrdiff_t` — is
visible at parse time. The include path is load-bearing: without the generated
`config.h` the compile aborts at the first `#include` and the stage silently
becomes a no-op, which is how the first version of it "passed" against a
deliberately planted 32-bit break.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Local multi-compiler preflight** (chosen) | Catches the real failures in seconds; no CI capacity used; works offline | Another script to keep in step with the workflow matrix | Chosen — validated against all three real breaks |
| Rely on CI | Nothing to maintain | Each miss costs a round-trip, and with one active PR that is queue time for everyone | This is the status quo that produced the three-hour block |
| Run the full matrix in the dev container | Closest to CI, includes HIP/SYCL | Minutes per run; too slow to sit in front of every commit | Kept for backend work; too heavy as a default |
| Cross-compile for Windows with MinGW | Real Windows compile | MinGW is not MSVC and misses `__declspec` differences; toolchain not installed | The static check covers the actual failure class more cheaply |
| Add more CI lanes | Central enforcement | Makes the queue slower, which is the problem being solved | Wrong direction |

## Consequences

- **Positive**: the three failure classes that cost round-trips on 2026-09-07
  now fail locally in seconds. Verified by planting each break and watching the
  matching stage fail, then reverting.
- **Positive**: `--stage NAME` makes it cheap to re-run just the lane that
  failed, and `--list` documents which CI context each stage stands in for.
- **Negative**: the stage list must be kept in step with the workflow matrix.
  `--list` is the single place that mapping is written down.
- **Negative**: `msvcism` is a set of pattern matches plus one scanner, not a
  compiler. It catches the
  known rejection classes, not everything MSVC dislikes; `Windows MSVC+*`
  remains the authority.
- **Neutral / follow-ups**: `Ubuntu HIP`, `Windows MinGW64` and the Windows
  MSVC lanes have no local equivalent here. The dev container (ADR-0451) covers
  HIP; the rest stay CI-only.
