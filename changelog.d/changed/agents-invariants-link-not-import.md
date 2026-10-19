- **Agent context no longer imports the rebase-invariants page
  ([ADR-1249](docs/adr/1249-praetor-governance-adoption.md), status update
  2026-10-12).** `AGENTS.md` and the compiled vendor context files keep the
  import line of `docs/development/agent-hard-rules.md` and link
  `docs/development/rebase-sensitive-invariants.md` instead of importing it.
  The page is 97 kB; a client that expands imports loaded it into every turn,
  which left a long session too little room to work after a context
  compaction. The page stays binding: read the section for the files you touch
  before a rebase, an upstream sync or a conflict resolution.
