---
paths:
  - scripts/release/verify-release-version.sh
  - scripts/release/tests/test-verify-release-version.sh
  - release-please-config.json
invariant: Publication preflight verifies manifest, single extra-files markers, tag commit, rollover cut.
---
<!-- markdownlint-disable MD013 MD060 -->
# Release version preflight (`verify-release-version.sh`)

## verify-release-version.sh

Every publication workflow checks out selected ordinary `vX.Y.Z` tag,
runs this preflight before any job receives write or OIDC permissions.
Script validates root manifest, discovers every coordinated marker
from `release-please`'s `extra-files`, requires exactly one matching
marker per file, confirms checked-out commit = selected tag. Add new
release surfaces to `extra-files`; never duplicate independent version
list in workflow.

Since ADR-1151, also proves fragment cut ran — nothing else did:
exactly one `## [X.Y.Z] - YYYY-MM-DD` heading in `CHANGELOG.md`;
`changelog.d/releases/X.Y.Z.json` receipt whose `.version` matches;
zero active fragments across six section directories; no
`_pre_fragment_legacy.md`; no surviving `release-as` / `bootstrap-sha`.
These mirror post-conditions of `rollover-changelog-fragments.sh`
exactly — change one, change other.
`concat-changelog-fragments.sh --check` is *not* substitute: passes
identically before and after cut.

Test coverage:
`scripts/release/tests/test-verify-release-version.sh`.
