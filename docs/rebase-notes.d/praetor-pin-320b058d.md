## Praetor pin 320b058d8cca (2026-10-17)

`chore/praetor-adopt-320b058d`, [ADR-3337](adr/3337-praetor-pin-320b058d-test-timeout.md). A
rebase or sync keeps `PRAETOR_REF` at `320b058d8cca...` in `.github/workflows/standards-gate.yml`.
`praetor-api.yml`, `praetor-docs.yml`, `.config/agent/hooks/block_evasion.py`,
`tools/apicompat/gate/main.go` and `tools/figures/` are praetor's text at the pin: take either side
and re-run `praetorctl adopt` (no `--force`). The figure player is under `tools/figures/player/`;
`tools/figures/dist/` and its `.gitignore` re-include do not come back. The `test-timeout` entry
of `.config/lint-exceptions.d/test-timeout.toml` expires on 2026-10-20 and leaves with the
Coverage Gate split; `.standards.yaml`'s block is regenerated with
`python3 scripts/ci/praetor_tidy_coverage.py --write`, never merged by hand.
