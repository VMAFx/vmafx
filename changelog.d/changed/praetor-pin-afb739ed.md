- **The praetor governance engine moves from `04cc813ff054` to `afb739ed81f3`
  ([ADR-2321](docs/adr/2321-praetor-pin-afb739ed.md)).** Praetor now lints every tracked
  nested `AGENTS.md` in the internal register; the 19 nested files and 195 `AGENTS.d/` pages
  that failed `praetorctl caveman check --kind=context` are rewritten, and the generated index
  header follows. The audit compares the declared SLSA Build Level (3) with the one the
  workflows reach (2): the gap is declared in `.config/lint-exceptions.d/HISS-11.toml`, which
  `scripts/ci/praetor_tidy_coverage.py` renders into `.standards.yaml` (expires 2027-01-04).
  Engine-written files regenerated: the Markdown gate lock (katex 0.19.0), the DevContainer
  bundle, `.paperclip/harness.json` and `rules.md`, the agent evasion hook; the register block
  no longer names skills the repository does not carry. The HISS baseline stays at 0.
