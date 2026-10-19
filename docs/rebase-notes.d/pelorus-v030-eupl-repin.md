## Pelorus mirror at v0.3.0, EUPL-1.2 (2026-10-09)

`chore/pelorus-revendor`, [ADR-2817](adr/2817-pelorus-v030-eupl-repin.md). A rebase or sync keeps
`PELORUS_VENDOR_SHA` at `e2e4040311a4...` and `PELORUS_MIRROR_LICENSE` at `EUPL-1.2` in
`scripts/sync-pelorus-interop.sh`, and never hand-merges the ten mirrored files: take either side and
run `scripts/sync-pelorus-interop.sh --update <pelorus checkout>`, then the check without `--update`.
`REUSE.toml` has no `core/src/interop/pelorus*` annotation and `.config/lint-exceptions.d/spdx.toml`
no Pelorus entry; a side that brings either back is stale. The `pelorus` entry of `docs/credits.yaml`
names `EUPL-1.2`; regenerate `docs/credits.md` with `make docs-fragments-write` after a conflict.
