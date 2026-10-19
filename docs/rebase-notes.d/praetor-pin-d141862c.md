## Praetor pin d141862c430b, the API gate's pinned checker module (2026-10-09)

`ci/apicompat-checker-xtools`, [ADR-3061](adr/3061-praetor-pin-d141862c.md). A rebase or sync keeps
`PRAETOR_REF` at `d141862c430b...` in `.github/workflows/standards-gate.yml` and praetor's text of
`tools/apicompat/gate/main.go` (its `checkerGoMod`, `checkerGoSum` and `checkerTools` constants
included). Regenerate the file with `adopt` in a throwaway copy, never by hand: `praetorctl audit`
locks it, and an older text brings back the checker that reads Go 1.27.2 packages as empty.
