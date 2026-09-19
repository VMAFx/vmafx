<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1263: `__HIP_PLATFORM_AMD__` is declared once by the build, not by each source

- **Status**: Proposed
- **Date**: 2026-09-19
- **Deciders**: Lusoris
- **Tags**: hip, build, meson, warnings, fork-local

## Context

`<hip/hip_runtime_api.h>` refuses to compile unless a platform macro is set, and the fork
supplied `__HIP_PLATFORM_AMD__` two different ways at once:

1. `-D__HIP_PLATFORM_AMD__=1` in `core/src/hip/meson.build`, but **only inside the
   `if not hip_runtime_dep.found()` fallback branch**.
2. A hand-written `#define __HIP_PLATFORM_AMD__ 1` at the top of eight host sources.

Both consequences are real. The macro lives in the implementation-reserved `__` namespace,
so every one of those eight files tripped `cert-dcl37-c` the moment a PR touched it — the
touched-file rule (ADR-0141 / ADR-1142) then made an unrelated change responsible for
cleaning it (bug ledger L-36). And the build-side define never covered the
`dependency('hip-lang')` branch at all, so on a ROCm that ships `hip-lang.pc` the whole HIP
build rested on the in-file copies. Nobody noticed because the two definitions are
textually identical (`1` both ways), which C permits silently, and because no ROCm in use
here ships that pkg-config file.

## Decision

`core/src/hip/meson.build` appends `declare_dependency(compile_args: ['-D__HIP_PLATFORM_AMD__=1'])`
to `hip_deps` **outside** the `if`, so both discovery branches get it from one place, and the
eight in-file `#define`s are deleted.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the per-file defines and add a `// NOLINT(cert-dcl37-c)` to each | No build change | ADR-0141 reserves NOLINT for load-bearing invariants, and a redundant macro is not one; it also leaves the `hip-lang` branch depending on source files rather than the build | Suppresses the symptom and keeps the coverage gap |
| Put the define in a shared fork header (`core/src/hip/common.h`) | One place, no meson edit | The macro must be set before the *first* `<hip/...>` include in every TU, so it only works if every file includes the shared header first — an ordering rule with no enforcement | Trades a visible duplication for an invisible ordering constraint |
| Add the define to the fallback branch only, as today, and fix the sources | Smallest diff | Leaves the `hip-lang` pkg-config branch with no definition at all once the in-file copies go — the build would break on exactly the configuration we cannot test here | Would turn a latent gap into a live break |
| Move it into the top-level `meson.build` project arguments | Global, simple | Applies `-D__HIP_PLATFORM_AMD__=1` to non-HIP translation units too | Wrong scope; `hip_deps` already names the right set |

## Consequences

- **Positive**: the macro has one definition site; touching any HIP host file no longer
  inherits a `cert-dcl37-c` finding; the `hip-lang` discovery branch is covered for the
  first time.
- **Negative**: a contributor adding a new HIP host source has to remember the macro comes
  from `hip_deps` rather than copy the `#define` from a neighbour. The meson block carries a
  comment saying so, and `core/src/feature/hip/AGENTS.md` records it.
- **Neutral / follow-ups**: verified by building the full HIP lane (`-Denable_hip=true`,
  ROCm 7.2.4, `/opt/rocm`) with every in-file define removed: 1,695 targets, exit 0.

## References

- `core/src/hip/meson.build` — the `hip_deps` declaration and its comment.
- Bug ledger `L-36` (`.workingdir/BUGS.md`).
- [ADR-0141](0141-touched-file-cleanup-rule.md), [ADR-1142](1142-whole-codebase-standards.md) —
  the touched-file rule that made the reserved-identifier finding blocking.
- Source: `req` — the user's direction to work through the bug ledger and fix every entry.
