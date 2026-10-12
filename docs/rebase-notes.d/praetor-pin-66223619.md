## Praetor pin 6622361940dc (2026-10-12)

`chore/praetor-pin-66223619`, [ADR-3285](adr/3285-praetor-pin-66223619.md). A rebase or sync keeps
`PRAETOR_REF` at `6622361940dc...` in `.github/workflows/standards-gate.yml`. The six register
skill files under `.agents/skills/` and `.claude/skills/` are praetor's text at that pin: take them
from one side of a conflict, or regenerate them with `adopt --lock-source-root=<praetor clone at the
pin>` in a throwaway clone; never re-wrap or edit them by hand. The launcher rules of the
`9bf502ec6ce5` entry still hold.
