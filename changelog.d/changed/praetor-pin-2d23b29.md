- **The praetor pin moves to `2d23b2924174`
  ([ADR-3333](docs/adr/3333-praetor-pin-2d23b29-persona-tiers.md)).** Every agent persona under
  `.agents/agents/` names a model tier, `flash` or `pro`, which `praetorctl compile-context`
  projects into each client's model name. The evasion hook also refuses admin merges through `gh`
  and the merge endpoints, and the API compatibility gate takes praetor's current text.
