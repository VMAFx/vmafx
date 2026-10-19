## Controller PostgreSQL store (2026-10-08)

`rc4/api-wp17-store`, [ADR-2350](adr/2350-cloud-native-platform.md). New fork-only package `cmd/vmafx-controller/store/`
(migrations, sqlc queries, generated `pgdb/`), `scripts/codegen/sqlc_generate.py`, the `SQLC_*` pins in
`build-config.env` and the Meson test `test_sqlc_generated_current`. Generated files take either side on a conflict and are
regenerated with `python3 scripts/codegen/sqlc_generate.py --write`. no upstream file.
