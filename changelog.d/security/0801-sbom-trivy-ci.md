- **SBOM generation (syft) + Trivy vulnerability scanning for Go binaries and container images (ADR-0801)** —
  Added `.github/workflows/supply-chain-sbom.yml` with three jobs:
  (1) `go-sbom-trivy`: builds all `cmd/*` Go binaries, generates SPDX-JSON + CycloneDX-JSON SBOMs via syft,
  and runs `trivy rootfs` per binary, gating on HIGH/CRITICAL findings;
  (2) `go-dep-trivy`: scans `go.mod`/`go.sum` transitive dependencies with `trivy fs --scanners vuln`,
  same HIGH/CRITICAL gate;
  (3) `container-sbom-trivy`: builds the dev-mcp image inline on PRs/push (or pulls the published GHCR CPU
  image on the weekly schedule) and scans with syft + `trivy image`.
  All SARIF results are uploaded to the GitHub Security tab; SBOMs are retained as workflow artifacts for
  90 days and attached to GitHub Releases via `supply-chain.yml`.
