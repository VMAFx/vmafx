<!-- markdownlint-disable MD013 MD060 -->
# ADR-1127: Use one independent SemVer release stream

- **Status**: Accepted
- **Date**: 2026-08-31
- **Deciders**: Lusoris, Codex (OpenAI)
- **Tags**: release, semver, automation, docs

## Context

VMAFx inherited a `vX.Y.Z-lusoris.N` suffix and later accumulated separate
release-please packages for the root project, AI trainer, development LLM, and
MCP server. That arrangement made ordinary patch releases hard to reason about,
allowed component tag collisions, and tied the fork's cadence to a slow upstream
project even when the fork shipped independently.

The repository already identifies its current source baseline as 3.2.0, while
the next requested release is a patch. Release publication also has a deliberate
human gate: release-please creates a draft release, and publishing that draft
creates the tag that fans out into the supply-chain and container workflows.

## Decision

VMAFx will use one independent ordinary SemVer stream, starting with `v3.2.1`.
The root release-please package owns libvmaf, the three Python package versions,
Helm `appVersion`, and the node image's pkg-config version. Helm chart packaging
and Rust crate versions remain independently versioned. Upstream alignment is
recorded in documentation and commits, not encoded in the release tag. The
release remains a draft until an authenticated operator publishes it, which is
the event that creates the tag and starts downstream publication workflows.
This decision supersedes [ADR-0011](0011-versioning-lusoris-suffix.md).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `vX.Y.Z-lusoris.N` | Encodes the upstream baseline in each tag | Non-standard ordering, verbose, couples fork identity to upstream | The fork now releases independently and consumers benefit from ordinary SemVer |
| Keep separate component release streams | Components can publish at different cadences | Colliding unqualified tags and ambiguous whole-project release fan-out | The shipped product is one coordinated distribution |
| One ordinary SemVer stream | Standard tooling, one release PR and tag, clear patch progression | Upstream baseline is no longer visible in the tag | Chosen; upstream provenance remains available in Git history and release notes |

## Consequences

- **Positive**: one release PR updates all release-owned versions and produces
  one unambiguous `vX.Y.Z` tag.
- **Negative**: consumers cannot infer the Netflix baseline from the tag alone.
- **Neutral / follow-ups**: publish the release-please draft to trigger signing
  and image workflows; remove the one-time `bootstrap-sha` after `v3.2.1` is
  published. Historical suffix examples remain historical evidence or legacy
  parser fixtures.

## References

- [release-please action documentation](https://github.com/googleapis/release-please-action)
- [GitHub Actions workflow-trigger documentation](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/trigger-a-workflow)
- [Research-1127](../research/1127-single-semver-release-stream.md)
- Source: `req` — "all this will be a .x patch of course, not a minor version"
- Source: `req` — "we should just use an one semver version and forget upstream"
