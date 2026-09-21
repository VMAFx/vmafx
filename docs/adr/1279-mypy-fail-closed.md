<!-- markdownlint-disable MD013 MD060 -->

# ADR-1279: Python type checking is a complete fail-closed gate

- **Status**: Accepted; supersedes [ADR-1261](1261-mypy-pre-push-delta-gate.md)
- **Date**: 2026-09-21
- **Deciders**: Lusoris
- **Tags**: ci, hooks, python, tooling, fork-local

## Context

ADR-1261 made the local type check a branch-delta ratchet because the repository
carried inherited findings, used a stale Python 3.10 analysis target, lacked
third-party type contracts, and ran hosted CI in advisory mode. That preserved
forward progress, but it also made known errors acceptable forever and let the
local and hosted contracts disagree. A changed import could expose an error in
an unchanged owned file without making that file part of the delta.

The repository now requires Python 3.14 and has enough maintained stubs plus
small repository-owned interface stubs to type-check the complete owned Python
surface. The remaining findings were code and test-contract defects, not a
reason to retain a baseline exemption.

## Decision

One canonical runner, `scripts/git-hooks/pre-push-mypy.py`, checks every tracked
`.py` and `.pyi` file below `ai/` and `scripts/` on every invocation. It runs
strict mypy for Python 3.14, partitions files only to preserve canonical package
identities, and returns non-zero for any diagnostic or tool/setup failure. Make,
pre-push, and the required hosted `Python Lint` job all call this runner. The
configuration contains no error-code suppressions, missing-import exemptions,
per-module ignores, merge-base comparison, touched-file narrowing, or committed
diagnostic baseline.

Packages without usable upstream typing expose the smallest interface the tree
actually consumes under `ai/typings/`. These are reviewed API contracts rather
than `Any`-only bypasses. Stub distributions needed by the checked imports are
ordinary pinned development dependencies installed by the same environment that
runs the gate.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Complete fail-closed gate (chosen) | One result locally and in CI; import changes cannot hide inherited errors; matches Python 3.14 | Requires fixing the existing tree and maintaining dependency contracts | — |
| Keep ADR-1261's merge-base delta | Cheap migration and unrelated branches can proceed around debt | Known errors remain accepted; local result depends on Git history; CI and local scope drift easily | The user explicitly rejected origin- and baseline-based exemptions |
| Check only production modules | Faster and avoids annotating dynamic tests | Test helpers and executable scripts can still carry broken contracts; import errors remain invisible | `ai/tests/` and operator scripts execute real repository behavior |
| Disable strict checks around untyped dependencies | Minimal stub maintenance | Converts missing knowledge into silent `Any` propagation | This recreates the blind spot the gate is meant to remove |

## Consequences

- **Positive**: every owned diagnostic blocks both a push and the required hosted job; all three entry points have identical scope and Python semantics.
- **Positive**: package-root grouping removes duplicate-module failures without excluding a source tree.
- **Negative**: the gate installs typed AI dependencies and checks unchanged files, so it takes longer than the former delta check.
- **Negative**: upgrades to Python, mypy, or a typed dependency may expose repository work that must be fixed in the same change.
- **Neutral / follow-ups**: code outside `ai/` and `scripts/` remains outside this specific runner until its package adopts the same contract; normal import following still checks dependencies reached by the owned roots.

## Supply-chain impact

- **New dependencies**: type-stub distributions used only by the `dev` / CI environment; exact package names and compatible lower bounds live in `ai/pyproject.toml`.
- **Repository contracts**: narrow `.pyi` interfaces for optional packages live in `ai/typings/` and ship no runtime code.
- **Runtime and CVE surface**: unchanged; this adds no production import, service, or network listener.

## References

- [ADR-1261](1261-mypy-pre-push-delta-gate.md) — superseded migration ratchet.
- [Research: fail-closed Python type-check gate](../research/mypy-fail-closed-2026-09-21.md).
- `scripts/git-hooks/pre-push-mypy.py` and `scripts/git-hooks/test-pre-push-mypy.py`.
- Source: `req` — "no warning or error is just ignored because of being og netflix code, fix them all".
