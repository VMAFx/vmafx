<!-- markdownlint-disable MD060 -->
# Research-0924 — Native bash pre-commit hook (opt-in)

**Date**: 2026-05-31
**Author**: lusoris / Claude
**Status**: implemented (this PR)

## Problem

The fork's local pre-commit gate runs through the pre-commit
framework (`pre-commit` driven by `.pre-commit-config.yaml`). Each
hook runs in an isolated, framework-managed Python virtualenv. The
isolation is the framework's strength — it makes a hook
self-contained, reproducible, and CI-portable — but it imposes a
per-hook startup cost of roughly ~3 s (venv activation + tool
import). For a contributor making many small commits per session,
that's the dominant component of `git commit` latency. The
modernization audit (ticket #16) flagged the gap.

We want the per-commit floor to drop to roughly the cost of running
the formatters directly, **without** changing what CI gates and
**without** removing the framework hook from the default flow.

## Goal

Drop local-commit overhead to ~0.4 s for the typical
"touched a Python file + a C file + a shell script" commit, while
keeping the framework hook as the default install and leaving CI
untouched.

## Constraints

- Default flow stays framework-based — opt-in only.
- Same file-scope rules as the framework hook (mirror excludes for
  `subprojects/`, `core/test/data/`, MATLAB sources, etc.).
- Graceful degrade on missing formatter binaries (don't block
  commits because the contributor lacks `shfmt`).
- Re-stage auto-fixes so the commit picks them up in the same op.
- Bypass via `git commit --no-verify` (standard escape hatch).
- `pre-push` PR-body validator (ADR-0108) must be installed by both
  paths — it's not specific to either pre-commit implementation.

## Options considered

### A — Replace the pre-commit framework outright with native bash

Pros: single hook path; no startup tax; one source of truth.

Cons: loses the wider framework matrix (gitleaks, semgrep,
conventional-commits, agent-worktree-drift guard, copyright
validator). Each of those would need a hand-rolled bash equivalent
plus a CI counterpart. The framework already gives CI a
deterministic install matrix (`pre-commit run --all-files` is the
canonical entry point); switching CI to a hand-rolled bash matrix
multiplies the maintenance surface.

**Rejected** — cost-benefit fails. The wider matrix is load-bearing
in CI; the local-commit win does not justify rewriting it.

### B — Speed up the framework itself by trimming hooks

Pros: keeps single source of truth.

Cons: the per-hook venv-activation cost is intrinsic to the
framework's isolation model. Removing hooks reduces total cost but
doesn't reduce the per-hook floor; even a single-hook config still
pays ~3 s of startup. And we don't *want* to remove the hooks — we
want them in CI.

**Rejected** — addresses the wrong dimension.

### C — Adopt a third hook manager (lefthook, husky)

Pros: lefthook is genuinely fast (Go binary, no venv wrap); husky is
ubiquitous in the JS ecosystem.

Cons: now there are three hook managers (framework, the new
manager, and any per-contributor wrappers). Adds a binary to the
distribution matrix and a new config syntax to learn. The fork's
contributors are already a small group; introducing a third tool
just to fix per-hook startup is overkill.

**Rejected** — adds tooling without removing tooling.

### D — Write the native helper in Go

Pros: cross-platform binary; matches the Phase 4 language
modernization direction (ADR-0702).

Cons: cold-start of a single-shot `go run` is ~150 ms; if we
distribute a built binary, we need to install it somewhere on PATH
or commit it to the repo. Bash is already on every contributor
machine; the ~0.4 s observed total for a 3-formatter commit is
well below the threshold where Go's startup would matter.

**Rejected** — premature for the scope. If we later want
cross-platform Windows-native hooks, revisit.

### E — Native bash + opt-in env var (chosen)

Pros:

- Zero impact on default flow — framework remains the default,
  CI unchanged.
- ~0.4 s total observed for a typical 3-formatter commit (vs ~9 s
  framework floor).
- Same `pre-push` install path — both modes wire in the ADR-0108
  validator identically.
- Graceful degrade on missing binaries (per-contributor toolchain
  variance is handled with one stderr line, not a blocked commit).
- File-scope rules can be unit-mirrored against
  `.pre-commit-config.yaml` excludes by inspection (small enough
  to keep in sync by review).

Cons:

- Two install paths to document and maintain.
- Native path misses the wider framework matrix locally — gitleaks,
  semgrep, conventional-commits surface on push / in CI instead of
  at commit time. Trade-off is acceptable because CI is the
  contract surface; commit time is convenience.

**Chosen.**

## Measurements (informal)

On a 12-core dev laptop with the framework venvs pre-warmed:

| Commit shape                         | Framework (`pre-commit`) | Native bash (`scripts/githooks/pre-commit.sh`) |
| ------------------------------------ | ------------------------ | ----------------------------------------------- |
| 1 Python file, 1-line edit           | ~3.1 s                   | ~0.18 s                                         |
| 1 C file + 1 Python file             | ~5.8 s                   | ~0.31 s                                         |
| 3-file mixed (.py + .c + .sh)        | ~8.7 s                   | ~0.42 s                                         |
| 10-file Python touch                 | ~3.4 s (ruff amortises) | ~0.41 s (ruff amortises)                        |

Bottleneck breakdown (perf rough):

- Framework path: ~85 % venv activation + tool import, ~15 %
  actual formatting.
- Native path: ~30 % shell overhead, ~70 % actual formatting.

The framework numbers don't include the cold-start case (first
commit after a `pre-commit autoupdate` or a CI cache miss), which
spikes to 30–90 s while it builds venvs.

## Implementation notes

- `scripts/githooks/pre-commit.sh` collects staged files via
  `git diff --cached --name-only --diff-filter=ACM`. Deletions /
  renames-into-non-existent are filtered.
- File dispatch is by extension + path prefix, mirroring the
  `files:` / `exclude:` selectors in `.pre-commit-config.yaml`.
  When the framework config changes scope, the native script needs
  a paired update — flagged as a follow-up in the ADR's
  Consequences section.
- The script uses `git hash-object` before/after to detect which
  files the formatter actually rewrote, then `git add` re-stages
  only those. This avoids unnecessary index churn on a no-op
  formatter run.
- Bypass via `--no-verify` works as standard.
- The installer (`scripts/githooks/install.sh`) preserves any
  existing non-symlink `pre-commit` / `pre-push` hook as
  `*.local-backup` rather than silently overwriting it. Matches
  the existing pre-push install semantics.

## Follow-ups

- If `.pre-commit-config.yaml` gains a new formatter that should
  run on every commit (e.g. `cargo fmt` once the Rust pilot
  lands), pair the addition with a new dispatch branch in
  `scripts/githooks/pre-commit.sh`.
- If the modernization plan introduces a cross-platform requirement
  (Windows-native dev with no bash), revisit option D (Go).

## References

- ADR-0924 (this PR) — the decision.
- ADR-0108 — pre-push deliverables validator, preserved across
  both install paths.
- ADR-0332 — agent worktree-drift guard, framework-only.
- ADR-0702 — Phase 4 language modernization (Go for production
  tools).
- Modernization audit ticket #16.
