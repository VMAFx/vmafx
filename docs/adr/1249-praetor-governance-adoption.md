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

Four forces make the adoption non-trivial here. Praetor's context transpiler
enforces a 300-line budget per generated vendor file, and `AGENTS.md` was 558
lines, so no projection could be produced at all. Since praetor `e4b35cb` the
engine also lints the whole of `AGENTS.md` in its internal ("caveman") register
and fails `compile-context --verify` and `audit` on prose, with no opt-out
(praetor ADR-0010). Praetor's lefthook configuration carries the governance
commands (`compile-context --verify`, `audit`, `state sync`), while
[ADR-0924](0924-native-pre-commit-hooks.md) deliberately rejected lefthook to
avoid a third hook manager and [ADR-1241](1241-worktree-hook-dispatch.md)
installs the pre-commit framework through its own dispatchers. And the repository
is mid-way to `1.0.0-rc.1`, so anything that rewrites product code competes
directly with the release.

## Decision

We will adopt praetor before the rc1 tag as **scaffolding and a ratchet only**:
the declarations, the compiled agent contexts, the generated policy artefacts and
one required CI gate land now, while the five pre-migration epic tasks are
post-rc1 work and no product code is refactored for them. `AGENTS.md` becomes a
harness in the internal register, about 250 lines, whose three heaviest rule sets
live in `docs/development/` pages, referenced by import lines so agents that
expand imports still receive the full text. The rules that only the hand-written
`CLAUDE.md` carried move into `AGENTS.md` and the hard-rules page, since
`CLAUDE.md` becomes a compiled projection. The reviewer personas under
`.claude/agents/` gain canonical sources in `.agents/agents/`, which the engine
projects to every vendor directory. Lefthook owns the `pre-commit` and `pre-push`
hooks and delegates both stages to the pre-commit framework, so the repository's
existing framework hooks keep running unchanged; the ADR-1241 dispatchers keep
`commit-msg` and `pre-rebase`. ADR-0924's native-hook escape hatch is retained
and this ADR supersedes only its "two hook managers is already one too many"
conclusion.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Adopt the whole epic (all five tasks) before rc1 | Repository is fully governed at the first release | Task 1 alone means refactoring every function above 60 LOC and clearing 538 HISS-01 control-flow infractions, mostly `goto`, across an 80k-LOC C/CUDA/SYCL/HIP tree; Task 3 swaps 213 dependencies for framework kits | Weeks of product-code churn competing with the release; the ratchet already prevents new debt without it |
| Keep the ADR-1241 dispatchers as the only hook owner and leave `lefthook.yml` uninstalled | No second hook manager; ADR-0924 and ADR-1241 unchanged; the `e4b35cb` audit accepts any installed `pre-commit` hook | The governance commands in `lefthook.yml` never run locally, so a stale projection, a prose `AGENTS.md` or a baseline increase is first seen in CI | The local gate is the point of adopting; the owner decided lefthook owns the hooks |
| Raise praetor's 300-line context budget instead of splitting `AGENTS.md` | No documentation moves | Every agent then loads ~560 lines of context per session in six files, and the budget exists precisely to prevent that | The split improves the harness regardless of praetor: the moved material is reference detail, not per-session reading |
| Compile a short stub `CLAUDE.md` and keep the long `AGENTS.md` | Smallest diff | The transpiler emits the full body by design; a stub is what the abandoned first attempt produced and it silently dropped the golden-data rule and the rebase invariants from Claude's context | Loses binding rules for the agent that does most of the work here |
| Move the `.claude/agents/` personas instead of copying them | One file per persona | `.claude/agents/` is a projection directory the engine rewrites, and Claude Code reads only that path | Byte-identical projection is what the engine verifies; a move changes nothing it checks |

## Consequences

- **Positive**: one canonical agent harness with six generated projections, so
  vendor context files can no longer drift; a debt baseline that can only
  shrink; the invariants and the rebase-sensitive index become linkable
  documentation pages instead of an unreadable 558-line file.
- **Negative**: a second hook manager to keep installed; praetor is pinned by
  commit rather than a release, so bumping it is a deliberate PR because a newer
  engine can tighten a policy and redden unrelated work, as `e4b35cb` did with the
  register lint and the two-way persona check; each reviewer persona now exists in
  five copies, of which only `.agents/agents/` may be edited; generated markdown
  has needed fixing to satisfy this repository's own markdownlint gate.
- **Neutral / follow-ups**:
  - `praetorctl sync --remote` must **not** be run against this repository: the
    branch name is hardcoded to `main` in the engine while this fork's default
    branch is deliberately `master` ([ADR-0002](0002-merge-path-master-default.md)).
    The generated `.github/rulesets/main.json` is therefore a local declaration
    only, and branch protection stays under the existing aggregator.
  - REUSE compliance is **not** part of this decision. The invalid SPDX
    identifiers and the missing licence texts were resolved on `master` by
    [ADR-1250](1250-eupl-fork-relicense.md) and
    [ADR-1255](1255-spdx-residual-identifier-correction.md); 6,087 files still
    carry no copyright information, and annotating them means classifying
    vendored third-party trees in its own reviewed change.
  - The five epic tasks (#1444–#1448) are scheduled after 1.0.0.
  - The CI gate runs the baseline-only `audit`, not `audit -base <ref>`. The
    `-base` mode adds a touched-file clean rule that reports 220 infractions on an
    ordinary change set here, because HISS-04 flags every function over 60 lines in
    any file a PR opens; it would block routine fixes to legacy C for reasons
    unrelated to the fix. Enable it once the baseline is low enough.
  - The engine is pinned by commit because the HISS count is a property of the
    engine build and of the scanned tree: the baseline is recorded from a clean
    clone, as CI checks one out, with the pinned engine.
  - Under CI the engine's hook gate only checks that `lefthook.yml` exists, so the
    CI job installs no hook toolchain.
  - `make install-hooks` ([ADR-1241](1241-worktree-hook-dispatch.md)) refuses to
    run once lefthook owns `pre-commit` and `pre-push`, because it treats those
    hooks as custom. Whether the installer should learn lefthook, or lefthook's
    installation should move behind it, is an open follow-up.
  - Praetor issues worth filing upstream: generated markdown should satisfy a
    standard markdownlint profile; `audit`, `plan` and the archetype disagree on
    the HISS-04 function-length limit (60 vs 100 vs 75); the editor capability
    scan in `adopt` stops at 4,096 files with no flag to raise it; `adopt` writes a
    placeholder `ruff.toml` that shadows `pyproject.toml` and is missing from its
    report. Measurements are in
    [the adoption research digest](../research/2064-praetor-adoption-measurements.md).

## References

- Supersedes the hook-manager conclusion of [ADR-0924](0924-native-pre-commit-hooks.md).
- [ADR-1241](1241-worktree-hook-dispatch.md) — the framework hook dispatchers
  lefthook now shares the hooks directory with.
- [ADR-0002](0002-merge-path-master-default.md) — `master` is the default branch.
- [ADR-1142](1142-whole-codebase-standards.md) — the existing whole-tree
  clang-tidy ratchet this baseline sits alongside.
- [ADR-1119](1119-golusoris-go-framework-adoption.md) — the framework adoption
  that epic task 3 would extend.
- Praetor ADR-0010 (text register per task) — the internal register the engine
  now enforces on `AGENTS.md`.
- Source: user direction, 2026-09-11 and 2026-09-15 (adopt the system before
  rc1; scaffolding and gate now, epic tasks after), and 2026-09-18 (refresh onto
  praetor `e4b35cb` with a scratch engine build; rewrite `AGENTS.md` in the
  internal register).
