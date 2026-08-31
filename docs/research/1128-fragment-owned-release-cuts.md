<!-- markdownlint-disable MD013 -->
# Research-1128: Fragment-owned release cuts

## Question

How can the first VMAFx SemVer release cut a large fragment-rendered
Unreleased block without duplicating or losing release notes?

## Evidence

- The pre-cut tree contains 411 added, 380 changed, 10 removed, 674 fixed, and
  22 security fragments plus the legacy source: 1,498 active inputs in total.
- A separate directory audit found 24 `chore` and three `refactor` fragments.
  The renderer emits a warning for those noncanonical directories but does not
  include their contents, so the apparent 1,498-input total was incomplete.
- `scripts/release/concat-changelog-fragments.sh` treats those inputs as
  canonical and rewrites only the Unreleased block. Keeping them after a
  release necessarily renders their entries again.
- A release-please 17.6 dry run using its normal changelog updater inserted a
  release section without consuming any fragment. Setting
  `skip-changelog: true` is the supported configuration for delegating that
  responsibility.
- The coordinated release has nine generic version files plus the root
  manifest. A rollover that validates only `CHANGELOG.md` can therefore cut
  notes from a stale release PR whose artifact versions disagree.
- Git retains deleted fragments permanently. A small JSON receipt containing
  the pre-cut commit, input count, and rendered SHA-256 is enough to audit the
  cut without maintaining a second exclusion database.

## Result

Keep fragment rendering as the sole changelog authority. Move the 27 historical
noncanonical fragments verbatim into `changed` before the first cut, preserving
their original section in each filename. Finalize a generated release PR with a
fail-closed script that checks the manifest and every generic marker, verifies
zero renderer drift, creates exactly one release heading, consumes all active
inputs, and writes a hash receipt. Test successful, idempotent, stale,
duplicate, invalid-input, drift, and empty-release paths in CI.

## Reproducer

```bash
bash scripts/release/tests/test-concat-changelog-fragments.sh
bash scripts/release/tests/test-rollover-changelog-fragments.sh
scripts/release/concat-changelog-fragments.sh --check
jq -e '.packages["."]."skip-changelog" == true' release-please-config.json
test "$(find changelog.d/chore changelog.d/refactor -type f -name '*.md' 2>/dev/null | wc -l)" -eq 0
```

## Sources

- [release-please manifest configuration](https://github.com/googleapis/release-please/blob/main/docs/manifest-releaser.md)
- [ADR-0221](../adr/0221-changelog-adr-fragment-pattern.md)
