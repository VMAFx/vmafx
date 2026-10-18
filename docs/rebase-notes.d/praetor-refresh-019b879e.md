## Praetor pin 019b879ec691, HISS-21 fixtures and the gitleaks `-m` rule (2026-10-18)

`chore/praetor-refresh-019b879e`. A rebase or sync keeps `PRAETOR_REF` at `019b879ec691...` in
`.github/workflows/standards-gate.yml`. `.gitattributes` keeps the `-text` lines for the 21 pinned
fixtures above the `praetor managed attributes` block, which must stay last; a new fixture that a
test pins by bytes needs its own line there. The Gitleaks step of
`.github/workflows/security-scans.yml` keeps `-m` in its `--log-opts` (the engine's audit refuses
a history scan without it, and `scripts/ci/tests/test_gitleaks_scan_scope.py` pins it).
`.github/ci-impact.json` routes `.mcp.json.license` and `opencode.json.license` with the files they
describe. The hook bundle's `_kill_bounded` keeps its guard before `os.killpg`.
