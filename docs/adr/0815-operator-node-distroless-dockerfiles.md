# ADR-0815: Distroless Dockerfiles for vmafx-operator and vmafx-node

<!-- markdownlint-disable MD013 -- ADR/research body text; pre-existing long lines per ADR-0864 tail -->

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `docker`, `ci`, `release`, `operator`, `node`, `k8s`, `phase4b`, `fork-local`

## Context

PR #152 added the vmafx-operator (`cmd/vmafx-operator`, ADR-0714) and the first
published vmafx-node Dockerfile (`docker/Dockerfile.node`, ADR-0717). That PR did not
include a dedicated `docker/Dockerfile.operator` — the operator binary was only built
locally with `go build`. Without a release-wired image the operator cannot be deployed
from a Helm chart or a `kubectl apply` manifest, and the node image (while the file
exists) was not yet published by any CI workflow.

Two decisions are needed:

1. **Operator image**: a `docker/Dockerfile.operator` that builds `cmd/vmafx-operator`
   as a pure-Go, CGO_ENABLED=0 binary into a `gcr.io/distroless/static-debian12`
   runtime. The operator never calls libvmaf directly (it drives Kubernetes CRD
   reconciliation); static-distroless is therefore the appropriate runtime — it is
   ~2 MB smaller than `distroless/cc-debian12` and has no libc in the attack surface.

2. **CI release pipeline**: a new workflow `docker-publish-operator-node.yml` that fires
   on `v*` tag pushes and `workflow_dispatch`, builds both images for `linux/amd64` +
   `linux/arm64` (QEMU for the node's CGO stages; BuildKit cross-compilation for the
   operator's pure-Go binary), and applies the same cosign + syft SBOM pipeline as
   `docker-publish-production.yml` (ADR-0698).

The node image (`docker/Dockerfile.node`) already existed and already used
`distroless/cc-debian12`. This ADR covers wiring it to the release CI — no
structural changes to the file itself.

## Decision

We will add `docker/Dockerfile.operator` and the CI workflow
`.github/workflows/docker-publish-operator-node.yml`.

- `Dockerfile.operator`: two stages — `golang:1.23-bookworm` builder (CGO_ENABLED=0,
  TARGETOS/TARGETARCH build args for native cross-compilation) → `distroless/static-debian12`
  runtime running as uid 65532 (distroless `nonroot`).
- `Dockerfile.node` (existing): no structural change; the workflow targets `--target node-cpu`
  (amd64 + arm64) for the released image; GPU variants remain manual or future-tracked.
- CI workflow: three jobs (`build-operator`, `build-node`, `smoke-test`) + a final
  aggregator gate. Operator job sets `TARGETARCH` build-arg per matrix platform; node
  job uses QEMU for arm64. Both jobs sign via cosign keyless OIDC and attest a
  CycloneDX SBOM.

Tag matrix for operator:

| Tag suffix         | Platforms       | Description           |
| ------------------ | --------------- | --------------------- |
| *(none)*           | amd64, arm64    | vmafx-operator        |

Tag matrix for node:

| Tag suffix         | Platforms       | Description           |
| ------------------ | --------------- | --------------------- |
| *(none)*           | amd64, arm64    | vmafx-node (CPU)      |

GPU node variants (`-cuda12`, `-rocm6`, `-sycl`) are out of scope for this ADR;
they are amd64-only and will be wired in a follow-on.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| `distroless/cc-debian12` for operator | Same base as node | Includes glibc — unnecessary for a CGO_ENABLED=0 binary; ~2 MB larger; wider libc attack surface | `distroless/static-debian12` is correct for static binaries |
| Inline operator build inside the node Dockerfile | Fewer Dockerfiles | Couples operator build time to the heavy ffmpeg/vmaf build stages; harder to cache | Separate file, faster CI, independent caching |
| Helm chart OCI artifact (no separate Dockerfile) | All-in-one | Helm OCI does not replace container images; the operator Deployment spec still needs an image reference | Must have a separate image |

## Consequences

- **Positive**: operator and node are both released as signed, SBOM-attested, multi-arch
  OCI images on every `v*` tag. Helm chart can reference `ghcr.io/vmafx/vmafx-operator`
  and `ghcr.io/vmafx/vmafx-node` with a real digest.
- **Negative**: two additional CI jobs per release (~15 min each). The node job builds the
  full ffmpeg stack twice (amd64 native + arm64 via QEMU), which is slow (~30 min arm64).
- **Neutral / follow-ups**: GPU node variants should be added in a follow-on PR; the
  `docker-publish-production.yml` pattern can be copied exactly.

## References

- ADR-0698: distroless base-image policy (production Dockerfile).
- ADR-0709: VMAFX Phase 4b distributed platform (parent scope).
- ADR-0713: vmafx-node Go worker binary.
- ADR-0714: vmafx-operator kubebuilder skeleton.
- ADR-0717: vmafx-node ffmpeg version policy + existing `docker/Dockerfile.node`.
- req: "add `Dockerfile.operator` and `Dockerfile.node` (the operator builds the Go binary `cmd/vmafx-operator`, the node builds `cmd/vmafx-node`). Both distroless per ADR-0698. Multi-arch amd64+arm64. Wire to release-please + CI to build on tag."
