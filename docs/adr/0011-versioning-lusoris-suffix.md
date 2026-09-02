<!-- markdownlint-disable MD013 MD060 -->
# ADR-0011: Version scheme v3.x.y-lusoris.N

- **Status**: Superseded by [ADR-1127](1127-single-semver-release-stream.md)
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: release, framework

## Context

The fork must version releases in a way that (a) makes the upstream Netflix version immediately obvious and (b) enumerates fork-side revisions independently. A pure semver scheme would collide with upstream; a timestamp scheme would hide the upstream baseline.

## Decision

We will use the version scheme `v3.x.y-lusoris.N`, where `3.x.y` tracks the Netflix upstream version and `N` is the fork's monotonically-increasing revision against that upstream baseline.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Pure semver from 0.1.0 | Independent history | Loses the upstream anchor | External users need to know upstream baseline |
| CalVer (`2026.04.17`) | Self-sorts chronologically | Hides upstream baseline | Same reason |
| `v3.x.y-lusoris.N` (chosen) | Upstream + fork visible | Longer string | The obvious mapping |

This decision was a default — no alternatives were weighed beyond the obvious ones listed.

## Consequences

- **Positive**: a release tag immediately tells readers which Netflix version it is built on and which fork revision applies.
- **Negative**: tags are long; tooling must parse the suffix.
- **Neutral / follow-ups**: release-please template encodes the format.

## References

- Source: `Q3.3`
- Related ADRs: ADR-0010
