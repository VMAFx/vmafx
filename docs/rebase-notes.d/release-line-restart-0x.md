## Release line restart at 0.1.0 (2026-10-12)

- `release-please-config.json`: root package `versioning: default`,
  `prerelease: false`, `bump-minor-pre-major: true`,
  `bump-patch-for-minor-pre-major: false`, one-shot `release-as: "0.1.0"`
  (removed by the rollover in the release-merge PR). **On sync**: upstream has
  no release-please; nothing to merge. Do not restore `versioning: prerelease`
  before a 1.0.0 candidate line opens (ADR-3292, ADR-1348).
- `scripts/release/check-one-shot-fields.sh` replaces the inline step of
  `.github/workflows/rule-enforcement.yml`; its test is
  `scripts/release/tests/test-check-one-shot-fields.sh`.
- Root `go.mod` retract block covers rc.1 to rc.3, one reason per entry.
- No engine source changes; SONAMEs unchanged.
