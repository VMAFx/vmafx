## Credits page and gate (2026-10-08)

`community-credits`, [ADR-2485](adr/2485-vmafx-credits-page.md).
`docs/credits.yaml`, `docs/credits.md`, `scripts/docs/{credits_lib,credits_checks}.py`,
`generate-credits.py` and `check-credits.py` are fork-authored. An upstream sync
that adds a vendored directory, notice file, font or foreign-copyright source
file must add its entry to `docs/credits.yaml` in the same change, or
`make docs-fragments-check` fails; the gate reads `REUSE.toml`, so keep its
annotations in step. Never hand-edit the tables of `docs/credits.md`.
