# ADR-0801: SBOM generation (syft) and Trivy vulnerability scanning for Go binaries and container images

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: ci, security, supply-chain, go, containers, sbom, trivy, fork-local

## Context

The fork ships six Go binaries (`vmafx-controller`, `vmafx-mcp`, `vmafx-node`,
`vmafx-operator`, `vmafx-server`, `vmafx-tune`) and several container images
(CPU, CUDA, ROCm, oneAPI, Vulkan, server). These artifacts were not covered by
any automated SCA (software composition analysis) or CVE-scanning gate. The
existing `supply-chain.yml` generates SBOMs at release time only, leaving the
day-to-day merge train unguarded.

The user requested a dedicated workflow that:
1. Generates syft SBOMs (SPDX-JSON + CycloneDX-JSON) for every Go binary and
   for container images, uploaded as workflow artifacts and to the GitHub
   Security tab via SARIF.
2. Runs Trivy and gates merges on HIGH or CRITICAL findings in either Go
   module dependencies or container OS packages.
3. Updates `CONTRIBUTING.md` so contributors can read the Security tab to
   understand findings.
4. Adds a `changelog.d/security/` fragment so the SBOM capability appears in
   release notes.

## Decision

Add `.github/workflows/supply-chain-sbom.yml` with three jobs:

- **`go-sbom-trivy`**: builds all `cmd/*` binaries, runs syft (SPDX + CycloneDX),
  runs `trivy rootfs` per binary. Gate: exits 1 on HIGH/CRITICAL. SARIF uploaded
  to Security tab under category `trivy-go-binaries`.
- **`go-dep-trivy`**: runs `trivy fs --scanners vuln` against `go.mod`/`go.sum`.
  Gate: exits 1 on HIGH/CRITICAL. SARIF under `trivy-go-deps`.
- **`container-sbom-trivy`**: on push/PR, builds the dev-mcp image inline; on the
  weekly schedule, pulls the published GHCR CPU image. Runs syft + `trivy image`.
  Gate: exits 1 on HIGH/CRITICAL. SARIF under `trivy-container`.

A `sbom-trivy-all` summary job makes `go-sbom-trivy` and `go-dep-trivy` hard
gates; the container job runs on all non-draft PR/push triggers.

Tool versions are pinned via `env:` constants (`SYFT_VERSION`, `TRIVY_VERSION`)
so a single-line bump PR suffices for updates. All action refs are SHA-pinned.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Extend existing `security-scans.yml` | One fewer workflow file | Already complex; Trivy semantics differ from semgrep/CodeQL; easier to reason about in isolation | Not chosen — separate file keeps concern boundaries clean |
| Grype instead of Trivy | Same syft ecosystem; native SARIF | Less mature container scanning; fewer CVE database sources | Not chosen — Trivy has broader OS CVE coverage |
| Only scan at release (extend `supply-chain.yml`) | Fewer CI minutes | Findings arrive too late — merge train already committed | Not chosen — gating on PRs catches regressions before merge |
| Advisory-only (no exit-code gate) | Never blocks merges | Findings accumulate silently | Not chosen — user explicitly requested HIGH/CRITICAL gate |

## Consequences

- **Positive**: HIGH/CRITICAL CVEs in Go deps or container OS packages are
  blocked at merge time. SBOMs are available as workflow artifacts (90-day
  retention) and as SARIF in the Security tab for every merge to master.
- **Negative**: Container build adds ~15 min to the weekly scan; the dev-mcp
  build on PRs adds ~10 min on push/PR paths.
- **Neutral / follow-ups**: The `go-sbom-trivy` and `go-dep-trivy` jobs are
  candidates for addition to the `required-aggregator.yml` required-checks
  list in a follow-up PR once the baseline noise level is understood.

## References

- req: "Add SBOM (syft) + Trivy scan to CI for the Go binaries + container images. Gate on HIGH/CRITICAL."
- [ADR-0263](0263-ossf-scorecard-green.md) — OSSF Scorecard remediation context
- [supply-chain.yml](../../.github/workflows/supply-chain.yml) — SLSA L3 + release-time SBOM
- [docker-publish-production.yml](../../.github/workflows/docker-publish-production.yml) — container build + cosign attest
