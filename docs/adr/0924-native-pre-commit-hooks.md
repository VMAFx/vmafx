<!-- markdownlint-disable MD013 MD060 -->
# ADR-0924: Native bash pre-commit hook as opt-in alternative to the pre-commit framework

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: build, ci, dx, tooling, fork-local, vmafx-modernization

## Context

The fork's local pre-commit gate runs through the
[pre-commit framework](https://pre-commit.com/) driven by
`.pre-commit-config.yaml`. The framework wraps every hook in an
isolated Python virtualenv that it manages itself; each hook spends
roughly ~3 s on venv activation and tool import before processing the
first file. For a typical small commit that touches a Python file,
a C file, and a shell script — three formatters — the wall-clock
floor on `git commit` is ~9 s before any actual work is done.

That cost is fine for CI (where the full hook matrix runs against the
whole tree and startup is amortised) but is the dominant component of
local commit latency for contributors who land many small commits per
session. The modernization audit ticket #16 flagged it as a
quick-win quality-of-life improvement.

Constraints:

- Must not displace the framework hook by default — CI continues to
  use the framework, and the framework's wider hook matrix (gitleaks,
  semgrep, conventional-commits, agent-worktree-drift guard,
  copyright check) is the project's source of truth for what "lint
  clean" means.
- Must be opt-in via a single switch, not a fork in the install
  procedure.
- Must mirror the framework hook's file-scope rules (same exclusions
  for `subprojects/`, `core/test/data/`, etc.) so a contributor
  using the native path doesn't drift from CI in surprising ways.
- Must degrade gracefully if a formatter binary is not on PATH
  (don't block commits because the contributor hasn't installed
  shfmt locally).

## Decision

Ship a native bash pre-commit hook at
`scripts/githooks/pre-commit.sh` plus an installer at
`scripts/githooks/install.sh` that wires either the framework hook
or the native hook into `.git/hooks/pre-commit` based on a single
environment variable (`VMAFX_NATIVE_HOOKS=1`). Both paths continue
to install the shared `pre-push` PR-body deliverables validator
(ADR-0108). The `Makefile` `hooks-install` target is renamed to
`install-hooks` (with `hooks-install` retained as a legacy alias)
and delegates to the installer. CI is unchanged.

The native hook runs only the three formatters that contributors hit
on every commit — `ruff check --fix`, `clang-format -i`, and
`shfmt -w -i 2 -ci` — with file-type and path-prefix dispatch that
mirrors the framework hook's scope. Missing binaries are reported on
stderr and skipped, never block the commit. Files the formatter
rewrites are re-staged automatically.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Replace pre-commit framework outright with native bash | Single hook path; no startup tax for any contributor | Loses gitleaks, semgrep, conventional-commits, agent-worktree-drift, copyright validators; would need to re-implement all of them in bash and keep them in sync with CI; large maintenance surface | Cost-benefit fails — framework's wider matrix is load-bearing in CI and pre-push |
| Speed up the framework by trimming hooks | Single source of truth | Doesn't address the per-hook venv-activation floor, which is intrinsic to the framework's isolation model | Doesn't solve the actual problem |
| Use [lefthook](https://github.com/evilmartians/lefthook) or [husky](https://typicode.github.io/husky/) as a third hook manager | Mature, faster than pre-commit, language-agnostic | Adds a third tool to the install matrix; lefthook is Go-binary distribution (one more thing to keep on PATH); husky is JS-ecosystem-heavy | Two hook managers is already one too many; adding a third is the wrong direction |
| Write the native helper in Go (consistent with Phase 4 language modernization) | Faster than bash; cross-platform binary | Cold-start of a single-shot `go run` is ~150 ms; build artefact would need distribution; bash is already on every dev machine | Bash matches the per-commit budget (~0.4 s total observed); Go's startup is not a bottleneck at this scope |
| Wrap the framework in a long-lived daemon (`pre-commit` has experimental `--daemon`) | Keep the framework as-is | Experimental flag; doesn't survive reboots; per-contributor lifecycle to manage | Out of scope; daemon lifecycle is its own complexity bucket |

## Consequences

- **Positive**:
  - Contributors who opt in see ~10x faster local commits (~0.4 s
    vs ~3 s for the equivalent file set).
  - Zero impact on contributors who don't opt in — framework
    remains the default and CI is unchanged.
  - Zero impact on CI gates — framework still runs against
    the full tree on every PR.
  - Existing `pre-push` PR-body validator is preserved across both
    paths via the same install script.
- **Negative**:
  - Two install paths to document and keep in sync (file-scope
    rules in the native script must mirror the framework's
    excludes).
  - Native path misses the wider framework matrix (gitleaks,
    semgrep, conventional-commits, etc.) locally — those findings
    surface on push / in CI instead of at commit time. Documented
    as a deliberate trade-off in
    `docs/development/pre-commit-hooks.md`.
- **Neutral / follow-ups**:
  - Future formatter additions to `.pre-commit-config.yaml` that
    should also run locally on every commit need a matching dispatch
    branch in `scripts/githooks/pre-commit.sh`. Updates should be
    paired in the same PR to avoid drift.
  - The `hooks-install` Makefile target is now an alias; existing
    `make hooks-install` invocations in dev scripts and AGENTS notes
    continue to work, but new references should use `install-hooks`
    (matches the more common verb-noun ordering).

## References

- Modernization audit ticket #16 (project_vmafx_phase4b_distributed_platform memory).
- ADR-0108 (deep-dive deliverables rule) — `pre-push` validator preserved across both paths.
- ADR-0332 (agent worktree-drift guard) — remains framework-only; contributors who care about it should stay on the framework path.
- `req`: user directive "add native bash pre-commit hooks as opt-in alternative to pre-commit framework. Pre-commit framework adds ~3s/hook for venv-wrap. Native is 10x faster."
