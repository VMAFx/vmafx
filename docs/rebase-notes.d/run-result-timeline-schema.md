## Run-result timeline schema skeleton (2026-11-10)

New files only; nothing upstream corresponds to them, so a rebase cannot
collide on their content. Two rebase-sensitive surfaces:

- `REUSE.toml` — the contributor annotation block sits directly after the
  first `[[annotations]]` catch-all. Other contributor pull requests (#3317
  and later) append their blocks at the same position; a conflict resolves by
  keeping both blocks, catch-all first and contributor blocks after it. Never
  fold a contributor block into the catch-all: the catch-all credits
  `2026 Lusoris`, and folding would misattribute contributor files to the
  maintainer (the credits gate reads the file headers against `REUSE.toml`).
- `api/run-result-timeline.schema.json` — the document evolves additively
  under HISS-14. A rebase that removes, renames or retypes a schema member,
  or reorders the `schema_version` contract, is a break of the additive
  contract, not a conflict to clean up; restore the schema side and reopen
  the discussion instead.
