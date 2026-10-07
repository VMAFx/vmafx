<!-- markdownlint-disable MD013 MD060 -->
# ADR-1589: the Helm chart deploys vmafx-controller as its own one-replica workload, and the release publishes a licence-gated controller image

- **Status**: Accepted (partially superseded by [ADR-2350](2350-cloud-native-platform.md) for the one-replica controller workload (Recreate strategy, ReadWriteOnce claim))
- **Date**: 2026-10-04
- **Deciders**: maintainer, agent
- **Tags**: helm, controller, node, operator, docker, release, supply-chain, phase4b, fork-local

## Context

`T-HELM-NO-CONTROLLER-WORKLOAD-2026-10-04`: the chart deployed no
`vmafx-controller`, and no workflow built `docker/Dockerfile.controller`. The
auth gateway, the tenant registry ([ADR-1519](1519-controller-tenant-registry.md)),
the node's controller client ([ADR-1524](1524-vmafx-node-controller-client.md))
and the operator's `GetJob` polling all need a controller; the only way to get
one was to put a self-built controller image into the *server* workload
(`image.repository`) with `auth.enabled`, and its gRPC port 9090 was not on the
chart's Service. The unbuilt Dockerfile linked Debian's `libvmaf-dev`
(amd64-only path) instead of the fork's library, had none of the licence
stages of [ADR-1513](1513-production-artifact-licensing.md), and the binary
had no `--version` although the docs said it had.

## Decision

- **Workload.** `controller.enabled` renders `<release>-controller`: a
  Deployment with exactly one replica and the `Recreate` strategy (the job
  queue is an embedded SQLite database, [ADR-1119](1119-golusoris-go-framework-adoption.md);
  two pods would own two queues), `VMAFX_DB_PATH=/data/vmafx-controller.db` on
  a `ReadWriteOnce` claim (`controller.persistence`, `existingClaim`, or an
  emptyDir when disabled), HTTP and gRPC probes on `/healthz` and `/readyz`,
  and a Service with `http` (8080) and `grpc` (9090).
- **Auth wiring.** `auth.*` is rendered into the controller only
  (`vmafx.controllerAuthEnv`, the ADR-1519 rules unchanged, plus the scoring
  roots of [ADR-1577](1577-scoring-paths-per-tenant.md)). `controller.enabled`
  and `auth.enabled` require each other. The server workload no longer gets
  auth settings, and an `image.repository` naming a `vmafx-controller` image is
  refused (breaking for the old form; migration in the Kubernetes guide).
- **Nodes and operator.** The nodes' `VMAFX_CONTROLLER_ADDR` defaults to the
  chart's Service (`node.controllerAddr` overrides); the operator gets
  `VMAFX_CONTROLLER_GRPC_ADDR` / `_HTTP_ADDR`. `node.controllerToken` and
  `operator.controllerToken` mount a Secret key read-only as
  `VMAFX_CONTROLLER_TOKEN_FILE` (re-read per call, ADR-1524; the operator side lands with the operator-auth change).
- **NetworkPolicies.** Controller default-deny, ingress to both ports from
  the namespace, operator → controller, node → the chart's controller pods on
  the gRPC port, controller → identity providers on 443 while auth is on, and
  the tenant-registry API-server rule moved to the controller pods (values key
  `serverToApiserver` kept).
- **Image.** `docker/Dockerfile.controller` follows `Dockerfile.go-server`
  stage for stage: the fork's libvmaf built from `core/`, the Go binary linked
  against it, licence notices and the `licence-check` receipt in the image,
  and a `source-export` stage; licence record `production-controller-image`
  reuses the go-server components through `rewrite`. `/data` belongs to the
  nonroot user. `docker-publish-operator-node.yml` builds it per architecture,
  publishes `ghcr.io/vmafx/vmafx-controller:<tag>` with cosign signature,
  provenance, CycloneDX SBOM, per-platform SPDX attestations and
  `<tag>-source`, and smoke-tests entrypoint, user, `--version` and `/readyz`.
  The controller gains `--version` (pkg/version, as the other binaries).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Own controller workload (chosen) | Server and controller can run side by side; the controller's ports, volume and single-replica strategy are its own; nodes and operator are wired by default | New values surface; the old `image.repository` form breaks | The old form ran a stateful single-writer service as a scalable stateless Deployment without its gRPC port |
| Switch the server workload's default image to the controller | Smallest chart change | `vmafx-server` users lose their image; the Deployment's replica count, strategy and Service stay wrong for a SQLite queue | Conflates two services |
| StatefulSet for the controller | Stable identity, volumeClaimTemplates | Still one replica; Recreate Deployment with one claim is simpler and is what the queue needs | No benefit at one replica |
| Separate controller Dockerfile written from scratch | Shorter file | Diverges from the licence-gated go-server build, the source of the drift that left the old file on distro libvmaf | Mirroring keeps one build recipe; the licence record is shared |

## Consequences

- **Positive**: a `helm install` with `controller.enabled` gives a working
  distributed platform (controller, nodes, operator) with auth, tokens and
  NetworkPolicies wired; the release publishes a signed, licence-checked
  controller image.
- **Negative**: releases that ran a controller through `image.repository`
  must move to `controller.enabled`; the queue database of the old Deployment
  is not migrated automatically. The new GHCR package needs its visibility
  checked after the first publish.
- **Neutral / follow-ups**: separate service accounts (only the controller
  reads `VmafxTenant`s) follow in `fix/helm-split-service-accounts`. A kind
  run of the full chart has not been made in this change; the renders, the
  image build and a local container run are the evidence.

## References

- [ADR-1513](1513-production-artifact-licensing.md), [ADR-1519](1519-controller-tenant-registry.md),
  [ADR-1524](1524-vmafx-node-controller-client.md), [ADR-1547](1547-helm-gpu-resource-name.md).
- `T-HELM-NO-CONTROLLER-WORKLOAD-2026-10-04`.
- Follow-up list of 2026-10-04, Lane PLAT item 1: "the Helm chart deploys the controller (workload, Service, the ports the node and operator use, auth wiring from #1997) and a controller image is built and published like the other production images, under ADR-1513's licence gate (notices, SBOM, `-source`), new GHCR package starts private: say so".
