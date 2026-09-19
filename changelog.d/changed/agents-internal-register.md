- **Agent-facing docs are written in the internal register.** Every
  `AGENTS.md` below the root, every skill under `.claude/skills/` and every
  reviewer persona now use the terse register ADR-1249 adopted for the root
  `AGENTS.md`: one fact per line, no articles, filler or hedges, with code,
  paths, identifiers, links and every rule kept verbatim. Each file was
  checked against its original with `praetorctl caveman floor`, which fails
  on any lost code span, command, identifier, link, directive or
  prohibition. User-facing documentation under `docs/` is unchanged.
