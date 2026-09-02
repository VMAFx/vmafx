<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1138: C translation units keep `NULL`; `modernize-use-nullptr` is scoped to C++

- **Status**: Accepted
- **Date**: 2026-09-02
- **Deciders**: lusoris
- **Tags**: `lint`, `ci`, `c23`, `quality-gate`, `rebase`

## Context

ADR-0692 raised the C standard to C23 (`-std=c23`, `/std:clatest` on MSVC).
Since LLVM 18 the clang-tidy check `modernize-use-nullptr` — enabled fork-wide
by ADR-0915 as the ratchet against raw `NULL` in the migrated `.cpp` files —
also runs on C23 translation units and proposes the `nullptr` keyword for
every typed null-pointer constant. The touched-file rule (ADR-0141) demands
zero clang-tidy warnings on any file a PR edits, so the first PR that reworks
an upstream-mirror C file (`core/src/libvmaf.c`, `core/src/predict.c`,
`core/src/feature/feature_collector.c` in the c-rework-core unit) has to decide
what a C TU does with ~70 such findings.

Two verified facts constrain the answer:

1. **Upstream parity.** Every one of these files carries the Netflix header and
   is periodically re-synced from Netflix/vmaf, whose sources spell the null
   pointer constant `NULL`. A keyword rewrite turns every future upstream hunk
   that touches a pointer initialiser into a merge conflict — exactly the class
   of rebase cost `docs/rebase-notes.md` exists to prevent. No C TU in the tree
   used `nullptr` before this ADR (verified with `find core -name '*.c' | xargs grep -l nullptr`).
2. **Compiler support is undocumented on a required lane.** Microsoft's
   `/std` reference and the MSVC conformance table list the C11/C17 features
   MSVC implements and describe `/std:clatest` as "all currently implemented
   ... features proposed in the next draft C standard" without enumerating
   `nullptr`. The required `Build — Windows (MSVC + CUDA)` check compiles
   `core/` with cl.exe, so introducing the keyword into C sources is an
   unverified bet against a required status check. GCC 13+ and Clang 16+
   accept it, which is why the local build never notices.

The CI lint lane (`clang-tidy-22 -p build --quiet <file> || fail`) only fails
on the `WarningsAsErrors` set, so these findings have been silently present on
every C TU with a typed `NULL` since the C23 bump — they simply had never been
discharged file by file.

## Decision

C translation units keep `NULL`. A C TU that a PR touches suppresses
`modernize-use-nullptr` with one file-scoped
`/* NOLINTBEGIN(modernize-use-nullptr) */` … `/* NOLINTEND(modernize-use-nullptr) */`
bracket whose inline comment states both constraints above and cites this
ADR (the ADR-0278 citation form). `.clang-tidy` is unchanged: the check keeps
ratcheting `.cpp` files exactly as ADR-0915 intended. The bracket is applied
file-by-file as files are touched under ADR-0141, not tree-wide.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Rewrite `NULL` → `nullptr` in touched C files | Zero suppressions; uses a C23 feature ADR-0692 unlocked | Every upstream sync re-conflicts on pointer initialisers; MSVC `/std:clatest` support for C `nullptr` is not documented and the Windows MSVC build is a required check; no C precedent in the tree | Unverifiable on a required lane and a permanent rebase tax |
| Disable `modernize-use-nullptr` in `.clang-tidy` | No per-file markers | Removes the raw-`NULL` ratchet from every migrated `.cpp` TU (ADR-0915's primary motivation) | Throws away the C++ coverage to solve a C-only artefact |
| `modernize-use-nullptr.NullMacros: ''` | Config-only | Same effect as disabling for `.cpp` files that spell `NULL` | Same loss as above |
| Per-directory `.clang-tidy` override | Scoped | `core/src/` mixes `.c` and `.cpp` in the same directories; clang-tidy cannot gate a check by source language | Not expressible |
| Leave the findings unaddressed | No change | Violates ADR-0141 for every touched C file; unbounded noise hides new findings | Non-compliant |
| File-scoped cited `NOLINTBEGIN/END` per touched C TU (chosen) | Keeps the `.cpp` ratchet, no build risk, zero rebase impact, self-documenting | One 6-line comment per C TU; must be kept balanced | Only option that satisfies ADR-0141, ADR-0915 and the Windows lane at once |

## Consequences

- **Positive**: touched C files reach zero clang-tidy warnings without
  introducing a keyword the Windows lane may reject; upstream syncs stay
  conflict-free on pointer initialisers; `.cpp` files keep the full
  `modernize-*` ratchet.
- **Negative**: C TUs will not be modernised to `nullptr` even where the
  compiler set would allow it. The bracket is a suppression, so a reviewer
  must check that a touched C file's `NOLINTBEGIN` is matched by `NOLINTEND`
  (clang-tidy reports an unmatched begin as an error, which is the backstop).
- **Neutral / follow-ups**: if Microsoft documents C `nullptr` under
  `/std:clatest` (or a `/std:c23` switch lands) and the fork accepts the
  upstream-sync cost, a superseding ADR can flip the policy and remove the
  brackets in one sweep. Until then, each C file reworked under ADR-0141 adds
  the bracket in the same PR.

## References

- [ADR-0692](0692-vmafx-c23-bump.md) — C23 bump (`nullptr` "available" in C
  TUs is a language statement, not a per-compiler verification).
- [ADR-0915](0915-clang-tidy-modernize-sweep.md) — `modernize-*` family
  enabled, scoped to `.cpp` TUs in practice.
- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file lint rule.
- [ADR-0278](0278-nolint-citation-closeout.md) — NOLINT citation form.
- Microsoft Learn, "/std (Specify Language Standard Version)" and "Microsoft
  C/C++ language conformance" (retrieved 2026-09-02): C-mode feature lists do
  not include `nullptr`.
- Research digest: [docs/research/1138-c23-nullptr-msvc-upstream-parity.md](../research/1138-c23-nullptr-msvc-upstream-parity.md).
- Source: orchestrated unit `c-rework-core` (rework of `core/src/libvmaf.c`,
  `core/src/predict.c`, `core/src/feature/feature_collector.c`,
  `core/src/read_json_model.cpp`) — the first ADR-0141 pass over these
  upstream-mirror TUs.
