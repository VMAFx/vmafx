# vmafx-operator container image

The vmafx-operator is the Kubernetes operator binary that reconciles the
`VmafxJob`, `VmafxNode`, and `VmafxModelTraining` custom resource definitions
(CRDs). It is published as a signed, SBOM-attested OCI image on every release tag.

ADR reference: [ADR-0815](../adr/0815-operator-node-distroless-dockerfiles.md),
[ADR-0714](../adr/0714-vmafx-operator-skeleton.md), and ADR-1129.

## Image coordinates

| Registry | Image | Default tag | Platforms |
| --- | --- | --- | --- |
| `ghcr.io` | `vmafx/vmafx-operator` | `latest` | `linux/amd64`, `linux/arm64` |

Pull by digest for production deployments:

```bash
docker pull ghcr.io/vmafx/vmafx-operator@sha256:<digest>
```

Verify the Sigstore signature:

```bash
cosign verify \
  --certificate-identity-regexp="https://github.com/VMAFx/vmafx/.github/workflows/docker-publish-operator-node.yml" \
  --certificate-oidc-issuer="https://token.actions.githubusercontent.com" \
  ghcr.io/vmafx/vmafx-operator@sha256:<digest>
```

## Build the image locally

```bash
docker build \
  -f docker/Dockerfile.operator \
  --target operator \
  --build-arg VMAFX_VERSION=dev \
  -t ghcr.io/vmafx/vmafx-operator:dev \
  .
```

Multi-arch (requires `docker buildx`):

```bash
docker buildx build \
  -f docker/Dockerfile.operator \
  --target operator \
  --platform linux/amd64,linux/arm64 \
  --build-arg VMAFX_VERSION=dev \
  -t ghcr.io/vmafx/vmafx-operator:dev \
  --push \
  .
```

Confirm the injected build version without Kubernetes credentials:

```bash
docker run --rm ghcr.io/vmafx/vmafx-operator:dev --version
```

## Run

```bash
docker run --rm \
  -e VMAFX_OPERATOR_METRICS_ADDR=:8080 \
  -e VMAFX_OPERATOR_HEALTH_PROBE_ADDR=:8081 \
  -e VMAFX_OPERATOR_LEADER_ELECTION=false \
  -e VMAFX_LOG_LEVEL=info \
  ghcr.io/vmafx/vmafx-operator:latest
```

In-cluster the operator reads kubeconfig from the service-account token
mounted by Kubernetes. The RBAC rules are managed by the Helm chart
([ADR-0699](../adr/0699-vmafx-helm-chart-k8s.md)); the image runs as
uid 65532 (`nonroot`) by default.

## Environment variables

| Variable | Default | Description |
| --- | --- | --- |
| `VMAFX_OPERATOR_METRICS_ADDR` | `:8080` | Prometheus `/metrics` endpoint bind address |
| `VMAFX_OPERATOR_HEALTH_PROBE_ADDR` | `:8081` | `/healthz` + `/readyz` bind address |
| `VMAFX_OPERATOR_LEADER_ELECTION` | `false` | Enable leader election for HA deployments |
| `VMAFX_LOG_LEVEL` | `info` | Log verbosity: `debug`, `info`, `warn`, `error` |

`--version` is the only process CLI switch; runtime configuration is supplied
through the environment variables above.

## Exposed ports

| Port | Protocol | Purpose |
| --- | --- | --- |
| 8080 | TCP | Prometheus metrics |
| 8081 | TCP | Health and readiness probes (`/healthz`, `/readyz`) |

## CI release pipeline

The workflow `.github/workflows/docker-publish-operator-node.yml` fires when a
GitHub release is published and on `workflow_dispatch`. It:

1. Builds `ghcr.io/vmafx/vmafx-operator` for `linux/amd64` + `linux/arm64`
   using BuildKit native cross-compilation (CGO_ENABLED=0 pure-Go binary; no QEMU
   needed).
2. Signs the pushed digest via `cosign sign --yes` (Sigstore keyless OIDC).
3. Generates a CycloneDX SBOM with `syft` and attaches it as a `cosign attest`
   predicate.
4. Uploads the SBOM JSON as a workflow artifact (90-day retention).
5. Attests GitHub-native build provenance for the pushed digest.
6. Verifies the signature, then asserts the image's `--version` output matches
   the published tag before the aggregator gate passes.

## Upgrade

Update the image tag (or digest) in the Helm `values.yaml`:

```yaml
operator:
  image:
    repository: ghcr.io/vmafx/vmafx-operator
    tag: "v3.2.1"
```

Then run `helm upgrade vmafx ./deploy/helm/vmafx -n vmafx-system`.
