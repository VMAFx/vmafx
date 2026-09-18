- **Praetor / HISS-16 governance scaffolding** (ADR-1249). The repository now
  carries a declarative governance manifest (`.standards.yaml`), a pinned
  standards lockfile, a technical-debt baseline that may only ratchet down, and
  one required CI context, `Standards & Invariant Verification Gate`, running
  `standardsctl audit` plus `standardsctl compile-context --verify` from praetor
  `e4b35cb`. `AGENTS.md` becomes the single canonical agent harness and is
  compiled into six vendor-specific context files — `CLAUDE.md`,
  `.cursor/rules/`, `.github/copilot-instructions.md`, `.windsurfrules`,
  `.gemini/GEMINI.md` and `.codex/rules.md` — so those may no longer be edited by
  hand; edit `AGENTS.md` and run `make compile-context`. `AGENTS.md` is written
  in the engine's internal register, which the gate lints; check an edit with
  `praetorctl caveman check AGENTS.md`. To fit the transpiler's 300-line budget
  per target, its hard rules and rebase-sensitive invariants move to
  [`docs/development/agent-hard-rules.md`](docs/development/agent-hard-rules.md)
  and
  [`docs/development/rebase-sensitive-invariants.md`](docs/development/rebase-sensitive-invariants.md),
  which the harness imports, together with the rules only the hand-written
  `CLAUDE.md` used to carry. The reviewer personas in `.claude/agents/` now have
  their source in `.agents/agents/`, which `make compile-context` projects to
  the Claude, Codex, Copilot and Gemini agent directories. `make verify-all`,
  `make audit` and `make compile-context` are the new entry points. Lefthook
  owns the `pre-commit` and `pre-push` hooks and delegates both to the existing
  pre-commit framework stages. The five pre-migration epic tasks are post-1.0.0
  work; this change adds no product-code refactoring.
