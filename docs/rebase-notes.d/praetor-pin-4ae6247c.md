## Praetor pin 4ae6247c57d2, the read-only context file and refreshed gate assets (2026-10-11)

`chore/praetor-adopt-4ae6247c`, [ADR-3240](adr/3240-praetor-pin-4ae6247c.md). A rebase or sync keeps
`PRAETOR_REF` at `4ae6247c57d2...` in `.github/workflows/standards-gate.yml` and praetor's text of
`AGENTS.readonly.md`, `tools/markdownlint/package.json` (with its lock file),
`tools/markdownlint/verify.mjs`, `.github/workflows/praetor-docs.yml`,
`.github/workflows/praetor-api.yml`, the register skills under `.agents/skills/` and
`.claude/skills/` (their `LICENSE` files included) and `.config/agent/hooks/block_evasion.py`.
Regenerate them with `adopt` in a throwaway copy, never by hand, and take generated files from one
side of a conflict: `praetorctl compile-context --verify` and `praetorctl audit` compare them byte
for byte. A branch that changes `AGENTS.md` runs `praetorctl compile-context` after the rebase, which
now also writes `AGENTS.readonly.md`. Keep its `text eol=lf` line in `.gitattributes`. The
`PreToolUse` entries `adopt` writes into `.claude/settings.json` and `.codex/hooks.json` stay out
until the operator decides on them.
