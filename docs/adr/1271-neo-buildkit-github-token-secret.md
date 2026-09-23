<!-- markdownlint-disable MD013 MD060 -->
# ADR-1271: Pass NEO GitHub credentials through optional BuildKit secrets

- **Status**: Proposed
- **Date**: 2026-09-20
- **Deciders**: kilian, Codex
- **Tags**: `build`, `container`, `security`, `supply-chain`, `ci`, `fork-local`

## Context

[ADR-1145](1145-neo-stack-derived-from-release.md) made the Intel NEO resolver
query GitHub release metadata and allowed authenticated requests by declaring
`ARG GITHUB_TOKEN`. Docker's native build check now reports
`SecretsUsedInArgOrEnv` for that declaration. Build arguments and environment
variables are inappropriate credential transports because they can be retained
in image metadata and provenance.

Authentication must remain optional. Public NEO releases build anonymously,
while authenticated API access avoids GitHub's lower shared-IP rate limit.
Raw Docker, Compose, and CI builds need one consistent transport without making
a token a prerequisite.

## Decision

Pass the optional token as the BuildKit secret `github_token`. The NEO fetch
instruction mounts it as `GITHUB_TOKEN` with `required=false` and forwards it to
the resolver only when non-empty. Compose sources the secret from the host's
`GITHUB_TOKEN`; raw Docker and CI use
`--secret id=github_token,env=GITHUB_TOKEN`. A raw build that omits the flag and
a Compose build with an unset or empty variable both remain anonymous. This
supersedes only ADR-1145's `ARG GITHUB_TOKEN` transport clause.

The contract is enforced by a repository checker with mutation tests and by
warning-fatal Docker and Compose `--check` steps before the full image build.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Optional BuildKit secret (chosen) | Ephemeral, native to the pinned Dockerfile frontend, one ID across raw Docker and Compose, preserves anonymous builds | Requires callers that want authentication to pass `--secret` | Chosen: smallest secure change with explicit optional semantics |
| Keep `ARG GITHUB_TOKEN` | Existing callers need no change | BuildKit rejects it; credential can enter image metadata or provenance | Violates the zero-warning and secret-handling contracts |
| Require the BuildKit secret | Simplifies the fetch command | Public anonymous builds fail when no credential is available | Rejects an existing supported build mode |
| Write the token to a Compose file secret | Works on older Compose clients | Requires a credential-bearing host file and cleanup lifecycle | The supported Compose version can source the environment directly |

## Consequences

- **Positive**: Docker build checks are warning-free; credentials exist only
  during one `RUN`; raw, Compose, and CI callers share the same secret ID.
- **Negative**: authenticated raw builds must add an explicit `--secret` flag,
  and Compose 2.15 or newer is required for build secrets (the documented
  prerequisite already exceeds that version).
- **Neutral / follow-ups**: the resolver's GitHub-host boundary, redirect
  stripping, bounded metadata reads, checksum verification, and anonymous
  behavior are unchanged.

## Supply-chain impact

- **New dependencies**: none.
- **Build-time fetches**: unchanged GitHub release metadata and checksum-pinned
  NEO packages.
- **CVE surface delta**: no new code executes in the image; secret exposure is
  narrowed from build metadata to one ephemeral mount.

## References

- [Docker build secrets](https://docs.docker.com/build/building/secrets/)
- [Compose build secrets](https://docs.docker.com/reference/compose-file/build/#secrets)
- [ADR-1145](1145-neo-stack-derived-from-release.md)
- [Research-2070](../research/2070-intel-neo-fetch-fail-closed.md)
- Source: req ("no ... warning or error is just ignored")
