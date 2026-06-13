<!-- markdownlint-disable MD013 MD060 -->
# ADR-0010: Sign release artifacts keyless via Sigstore

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: security, release, supply-chain

## Context

Release artifacts need a verifiable provenance chain. Managed signing keys (GPG, cosign key-pair) require rotation, secret storage, and a revocation procedure. Keyless signing via Sigstore leverages GitHub's OIDC identity and the public Rekor transparency log, eliminating key management entirely.

## Decision

We will sign release artifacts keyless via GitHub OIDC + Sigstore, with no managed keys on the fork's side.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Managed GPG key | Familiar; offline-verifiable | Secret storage + rotation burden; single point of failure | Operational cost too high for solo-dev fork |
| Managed cosign key-pair | Works with OCI registries | Same secret-management burden | Same reason |
| Sigstore keyless (chosen) | No secrets; transparency log; standard OSS practice | Depends on Sigstore availability + GitHub OIDC | Rationale note: eliminates key-management operational burden; transparency log provides non-repudiation without private keys; standard for modern OSS |

## Consequences

- **Positive**: no secrets to rotate or lose; signatures are publicly auditable via Rekor.
- **Negative**: verification depends on Sigstore infrastructure; offline verification requires cached material.
- **Neutral / follow-ups**: `/prep-release` skill validates Sigstore + OIDC prerequisites.

## References

- Source: `Q3.4`
- Related ADRs: ADR-0011
