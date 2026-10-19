- **The praetor pin moves to `6622361940dc`
  ([ADR-3285](docs/adr/3285-praetor-pin-66223619.md)).** CI and the governance hooks run praetor's
  current main. The register skills `adhd-format`, `caveman` and `social-text` under
  `.agents/skills/` and `.claude/skills/` are praetor's text at that commit, wrapped so they pass
  markdownlint's default rules; their content is unchanged. `praetorctl audit` reports the same
  warnings as at `9bf502ec6ce5`.
