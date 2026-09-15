<!-- markdownlint-disable MD013 MD060 -->
# ADR-1249: Adopt praetor governance, with lefthook owning the git hooks

- **Status**: Proposed
- **Date**: 2026-09-15
- **Deciders**: lusoris
- **Tags**: ci, process, agents, tooling, governance, docs, workspace

## Context

The workstation topology contract this fork now lives under requires every
active repository to be adopted into the praetor standards system
(`.standards.yaml`, `.standards.lock`, `.standards-baseline.json`). Praetor
declares its invariants as HISS-01..HISS-16, compiles one canonical `AGENTS.md`
into six vendor-specific agent context files, and gates a repository on a debt
baseline that may only ratchet down. A pre-migration epic and five task issues
were opened against this repository to track it.

Three forces make the adoption non-trivial here. Praetor's context transpiler
enforces a 300-line budget per generated vendor file, and `AGENTS.md` was 558
lines, so no projection could be produced at all. Praetor's own audit requires
active git hooks installed by lefthook, while [ADR-0924](0924-native-pre-commit-hooks.md)
deliberately rejected lefthook to avoid a third hook manager alongside the
pre-commit framework and the native bash hooks. And the repository is mid-way to
`1.0.0-rc.1`, so anything that rewrites product code competes directly with the
release.

## Decision

We will adopt praetor before the rc1 tag as **scaffolding and a ratchet only**:
the declarations, the compiled agent contexts, the generated policy artefacts and
one required CI gate land now, while the five pre-migration epic tasks are
post-rc1 work and no product code is refactored for them. `AGENTS.md` becomes a
265-line harness whose three heaviest rule sets live in `docs/development/`
pages, referenced by import lines so agents that expand imports still receive
the full text. Lefthook owns `.git/hooks` because praetor's audit requires it,
and delegates to `pre-commit run` so the repository's existing 35 framework
hooks keep running unchanged; ADR-0924's native-hook escape hatch is retained
and this ADR supersedes only its "two hook managers is already one too many"
conclusion.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Adopt the whole epic (all five tasks) before rc1 | Repository is fully governed at the first release | Task 1 alone means refactoring every function above 60 LOC and removing 576 `goto` statements across an 80k-LOC C/CUDA/SYCL/HIP tree; Task 3 swaps 213 dependencies for framework kits | Weeks of product-code churn competing with the release; the ratchet already prevents new debt without it |
| Keep the pre-commit framework as hook owner and skip lefthook | No third hook manager; ADR-0924 unchanged | Praetor's audit gate reports the repository as ungoverned, so the required CI check can never pass | Defeats the purpose of adopting; the audit's hook check is not configurable |
| Raise praetor's 300-line context budget instead of splitting `AGENTS.md` | No documentation moves | Every agent then loads ~560 lines of context per session in six files, and the budget exists precisely to prevent that | The split improves the harness regardless of praetor: the moved material is reference detail, not per-session reading |
| Compile a short stub `CLAUDE.md` and keep the long `AGENTS.md` | Smallest diff | The transpiler emits the full body by design; a stub is what the abandoned first attempt produced and it silently dropped the golden-data rule and the rebase invariants from Claude's context | Loses binding rules for the agent that does most of the work here |

## Consequences

- **Positive**: one canonical agent harness with six generated projections, so
  vendor context files can no longer drift; a debt baseline that can only
  shrink; the invariants and the rebase-sensitive index become linkable
  documentation pages instead of an unreadable 558-line file.
- **Negative**: a second hook manager to keep installed; praetor is pinned by
  commit rather than a release, so bumping it is a deliberate PR because a newer
  engine can tighten a policy and redden unrelated work; generated markdown has
  needed fixing to satisfy this repository's own markdownlint gate.
- **Neutral / follow-ups**:
  - `praetorctl sync --remote` must **not** be run against this repository: the
    branch name is hardcoded to `main` in the engine while this fork's default
    branch is deliberately `master` ([ADR-0002](0002-merge-path-master-default.md)).
    The generated `.github/rulesets/main.json` is therefore a local declaration
    only, and branch protection stays under the existing aggregator.
  - REUSE compliance is **not** part of this decision. 6,192 of 8,256 files
    carry no copyright information, and 710 fork-added files declare
    `BSD-3-Clause-Plus-Patent`, which is not a valid SPDX identifier while the
    root `LICENSE` is `BSD-2-Clause-Patent`. Correcting those identifiers
    asserts a licence and needs its own reviewed change.
  - The five epic tasks (#1444–#1448) are scheduled after 1.0.0.
  - The CI gate runs the baseline-only `audit`, not `audit -base <ref>`. The
    `-base` mode adds a touched-file clean rule that reports 220 infractions on an
    ordinary change set here, because HISS-04 flags every function over 60 lines in
    any file a PR opens; it would block routine fixes to legacy C for reasons
    unrelated to the fix. Enable it once the baseline is low enough.
  - The engine is pinned by commit because the HISS count is a property of the
    engine build: two builds measured this same commit deterministically and
    disagreed (1,687 against 1,688). An unpinned engine would move the ratchet on
    its own.
  - Praetor issues worth filing upstream: generated markdown should satisfy a
    standard markdownlint profile; `audit`, `plan` and the archetype disagree on
    the HISS-04 function-length limit (60 vs 100 vs 75); the flavor gate fails in
    a fresh worktree because the private `.workingdir/` ledgers are gitignored.

## References

- Supersedes the hook-manager conclusion of [ADR-0924](0924-native-pre-commit-hooks.md).
- [ADR-0002](0002-merge-path-master-default.md) — `master` is the default branch.
- [ADR-1142](1142-whole-codebase-standards.md) — the existing whole-tree
  clang-tidy ratchet this baseline sits alongside.
- [ADR-1119](1119-golusoris-go-framework-adoption.md) — the framework adoption
  that epic task 3 would extend.
- Source: user direction, 2026-09-11 and 2026-09-15 (adopt the system before
  rc1; scaffolding and gate now, epic tasks after).
