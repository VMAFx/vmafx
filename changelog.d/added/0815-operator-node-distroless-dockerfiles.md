## Added

- **`docker/Dockerfile.operator`**: multi-stage distroless image for the vmafx-operator
  Kubernetes controller. Builder: `golang:1.23-bookworm` (CGO_ENABLED=0); runtime:
  `gcr.io/distroless/static-debian12` running as `nonroot` (uid 65532). Exposes
  Prometheus metrics (:8081) and health-probe (:8082) ports. Multi-arch amd64 + arm64
  via BuildKit native cross-compilation. ADR-0815.
- **`.github/workflows/docker-publish-operator-node.yml`**: CI workflow that fires on
  `v*` release tags (and `workflow_dispatch`). Builds and pushes both
  `ghcr.io/vmafx/vmafx-operator` and `ghcr.io/vmafx/vmafx-node` (CPU variant,
  amd64 + arm64), signs each digest via cosign keyless OIDC, attaches a CycloneDX SBOM
  via `cosign attest`, and runs a binary smoke-test before the aggregator gate passes.
  Mirrors the `docker-publish-production.yml` pattern (ADR-0698). ADR-0815.
- **`docs/backends/operator.md`**: operator image runbook — build, push, run, and
  upgrade instructions for the vmafx-operator container. ADR-0815.
