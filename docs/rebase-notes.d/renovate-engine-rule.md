## Renovate rule for the praetor engine (2026-10-29)

`chore/renovate-engine-rule`. A rebase or sync keeps the last `packageRules`
entry in `renovate.json`, described as "praetor-managed engine", with
`enabled: false` and its five package names. The praetor engine's
`renovate-engine-bumps` audit fails when the entry is missing or its names
differ. The praetor pin in `.github/workflows/standards-gate.yml` does not
change. No score, C API or build change.
