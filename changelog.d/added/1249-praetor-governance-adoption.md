- **Praetor / HISS-16 governance scaffolding** (ADR-1249). The repository now
  carries a declarative governance manifest (`.standards.yaml`), a pinned
  standards lockfile, a technical-debt baseline that may only ratchet down, and
  one required CI context, `Standards & Invariant Verification Gate`, running
  `standardsctl audit` plus `standardsctl compile-context --verify`. `AGENTS.md`
  becomes the single canonical agent harness and is compiled into six
  vendor-specific context files — `CLAUDE.md`, `.cursor/rules/`,
  `.github/copilot-instructions.md`, `.windsurfrules`, `.gemini/GEMINI.md` and
  `.codex/rules.md` — so those may no longer be edited by hand; edit `AGENTS.md`
  and run `make compile-context`. To fit the transpiler's 300-line budget per
  target, the harness drops from 558 to 265 lines: its hard rules and
  rebase-sensitive invariants move to
  [`docs/development/agent-hard-rules.md`](docs/development/agent-hard-rules.md)
  and
  [`docs/development/rebase-sensitive-invariants.md`](docs/development/rebase-sensitive-invariants.md),
  which the harness imports, and the worktree section now points at the existing
  worktree-discipline page. `make verify-all`, `make audit` and
  `make compile-context` are the new entry points. Lefthook owns `.git/hooks`
  because the audit requires it and delegates to the existing pre-commit
  framework hooks. The five pre-migration epic tasks are post-1.0.0 work; this
  change adds no product-code refactoring. Five licence texts the tree's own
  SPDX tags already referenced are added under `LICENSES/`.
