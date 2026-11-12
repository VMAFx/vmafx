---
paths:
  - scripts/release/rollover-changelog-fragments.sh
  - scripts/release/tests/test-rollover-changelog-fragments.sh
  - .pre-commit-config.yaml
invariant: Release-PR cut consumes fragments, retires one-shot fields, writes SHA-256 receipt.
---
<!-- markdownlint-disable MD013 MD060 -->
# Release-PR changelog rollover (`rollover-changelog-fragments.sh`)

## rollover-changelog-fragments.sh

Release-PR-only operation. Preconditions deliberately strict: working
tree clean; manifest and every generic version marker equal requested
version; fragment rendering current; target heading absent; at least
one active source exists; any remaining `release-as` matches requested
version. After cut: active fragments and `_pre_fragment_legacy.md`
removed, one-time `release-please` fields retired; exact rendered
content remains in versioned CHANGELOG section, source history remains
in Git.

Generated receipt records source count and rendered SHA-256. Second
identical invocation = no-op only when target heading and receipt
exist, and no active sources or cutover fields remain. Late merge
after rollover adds active fragment again, invalidating release PR
until operator rebuilds cut.

Version shape = verifier's shape, no wider: `X.Y.Z` or `X.Y.Z-rc.N`
(ADR-1201). RC cut = own dated section + `releases/X.Y.Z-rc.N.json`
receipt; rc.1 cut retires one-shot fields, as `verify-release-version.sh`
demands at every tag. Marker extractor keeps optional `-rc.N` group, same
as verifier; without it `1.0.0-rc.1` marker reads `1.0.0`, mismatches.

Archive path `docs/changelog-archive/X.Y.Z.md` exempt from 1 MB
`check-added-large-files` gate (ADR-1345): exclusion
`^docs/changelog-archive/[^/]+\.md$` in `.pre-commit-config.yaml`. Move
archive path -> move exclusion in same PR; never widen it. T18 pins both.

Test coverage:
`scripts/release/tests/test-rollover-changelog-fragments.sh` (T14 runs
verifier against RC cut).
