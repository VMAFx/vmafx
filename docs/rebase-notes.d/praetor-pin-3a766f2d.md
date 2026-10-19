## Praetor pin 3a766f2d56ad, the REUSE workflow and the HISS-10 entries (2026-10-08)

`chore/praetor-pin-3a766f2d`, [ADR-2784](adr/2784-praetor-pin-3a766f2d.md). A rebase or sync keeps
`PRAETOR_REF` at `3a766f2d56ad...` in `.github/workflows/standards-gate.yml` and the engine's texts of
`praetor-api.yml`, `praetor-docs.yml`, `tools/apicompat/gate/` and `tools/figures/` (regenerate with
`adopt` in a throwaway copy, never hand-edit). `.github/workflows/reuse.yml` is praetor's rendering with
both actions pinned to commits; keep the pins, and keep `REUSE lint` in the aggregator's `required`
and `strictMustReport` lists, in `always` and `untiered_jobs` of `.github/ci-tier.json`, and
`reuse-lint` in the Pre-Commit job's `SKIP`. The `exceptions:` block of `.standards.yaml` is rendered
from `.config/lint-exceptions.d/` (`HISS-10.toml` and `HISS-11.toml` through `PRAETOR_RULES`); on a
conflict take either side and run `python3 scripts/ci/praetor_tidy_coverage.py --write`.
