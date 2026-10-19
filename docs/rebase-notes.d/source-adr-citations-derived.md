## Source ADR citations: live bindings derived, registry keeps retired and fixtures (2026-10-07)

`ci/citations-derived`, [ADR-2200](adr/2200-source-adr-citations-derived.md). Fork-only gate. `scripts/ci/source-adr-citations.json` is
schema 2 with `retired` and `fixtures` only; on a conflict in it take master's side and keep only the hand-governed records your
branch changed (a retired or fixture site count). A `live` key is an error: delete it, never re-add `--write`. An upstream sync
that cites an ADR number needs no registry edit.
