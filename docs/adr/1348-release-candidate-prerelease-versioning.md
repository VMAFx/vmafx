<!-- markdownlint-disable MD013 MD060 -->
# ADR-1348: Number release candidates with release-please's prerelease versioning

- **Status**: Accepted
- **Date**: 2026-09-27
- **Deciders**: lusoris
- **Tags**: release, ci

## Context

[ADR-1201](1201-release-candidates-before-1-0-0.md) cuts release candidates before the final 1.0.0, and [ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md) orders them: RC1 for correctness, RC2 for benchmarks and tuning, RC3 for the retrain. `release-please-config.json` set `"versioning": "default"` with `"prerelease": true` and `"prerelease-type": "rc"`.

Once v1.0.0-rc.1 was published, the fix commits that followed made release-please open release PR #1575 as **1.0.1-rc.1**. The default strategy applies a patch bump to `1.0.0-rc.1` and keeps the prerelease identifier, so it never counts candidates. #1568 documented a workaround: a manual `Release-As: 1.0.0-rc.2` footer on the last commit merged before each later cut. Forgetting it would publish the wrong version under an immutable tag.

In release-please 17.6 (the library the pinned `release-please-action` v5.0.0 depends on), `src/versioning-strategies/prerelease.ts` handles a version that already carries a prerelease identifier as follows:

- **Fix commits**: `PrereleasePatchVersionUpdate` increments the prerelease number.
- **Features**: `PrereleaseMinorVersionUpdate` does the same while `patch` is 0.
- **Breaking changes**: `PrereleaseMajorVersionUpdate` does the same while `minor` and `patch` are 0.

On `1.0.0-rc.N` all three therefore produce `1.0.0-rc.N+1`. When `prerelease` is `false`, the strategy keeps only `major.minor.patch` of that bump, so the final cut becomes `1.0.0`. `src/factory.ts` passes the package's `prerelease` and `prerelease-type` settings to the strategy.

## Decision

Set `"versioning": "prerelease"` for the root package. Release candidates are numbered `1.0.0-rc.2`, `1.0.0-rc.3`, … without a manual footer, and the final cut needs only `"prerelease": false`. The `Release-As: X.Y.Z` commit footer stays available as a one-shot override, but a routine candidate cut no longer needs it. The release PR remains a standing proposal: it is merged only when the next candidate is due.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `versioning: prerelease` (chosen) | Candidates are counted automatically; the final 1.0.0 needs no version override; verified in the pinned library's source | The version depends on release-please's strategy code, which a major upgrade could change | The Release Script Contract job and the release PR title surface a change before any cut |
| Keep `versioning: default` with a `Release-As: 1.0.0-rc.N` footer before each cut (#1568) | No config change | A manual step on every candidate; forgetting it tags `1.0.1-rc.1`, which cannot be taken back | Error-prone for 4–8 candidates |
| Pin `release-as` in the config per candidate | Explicit | `release-as` is persistent, swallows later bumps, and the release script contract forbids it after the first cut | Rejected by the existing contract |

## Consequences

- **Positive**: release PR #1575 retitles itself to 1.0.0-rc.2 on the next push to master; later candidates need no manual version step.
- **Negative**: none known; a release-please upgrade that changes the strategy would show in the release PR title before any cut.
- **Neutral / follow-ups**: `docs/development/release.md` and `scripts/release/AGENTS.md` drop the footer procedure; the Release Script Contract job in `rule-enforcement.yml` fails while the manifest is a release candidate and the root package's `versioning` is not `prerelease`.

## References

- popup, 2026-09-27: "Switch to prerelease versioning (Recommended)" (maintainer answer to how the next candidate gets 1.0.0-rc.2).
- [ADR-1201](1201-release-candidates-before-1-0-0.md) — release candidates before 1.0.0.
- [ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md) — RC1/RC2/RC3 sequence.
- release-please v17.6.0 source: `src/versioning-strategies/prerelease.ts`, `src/factories/versioning-strategy-factory.ts`, `src/factory.ts`.
