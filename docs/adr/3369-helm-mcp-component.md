<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3369: The Helm chart deploys vmafx-mcp from the node image

- **Status**: Accepted
- **Date**: 2026-10-24
- **Deciders**: Lusoris (operator orders vmafx-chart-mcp-component-20261024 and vmafx-chart-mcp-image)
- **Tags**: helm, kubernetes, mcp, docker

## Context

`deploy/helm/vmafx` deploys the controller and the node pool but not the Go
MCP server `cmd/vmafx-mcp`, the thin client of the controller's Client API
that MCP hosts talk to. Running it in a cluster meant a hand-written
Deployment. No published image carried the binary: `docker/Dockerfile.node`
built only `vmafx-node`, and `vmafx-mcp` needs cgo, so a static scratch image
was not an option without a second Go build stage and a second licence
inventory. The MCP HTTP transport refuses to start without a token unless
`VMAFX_MCP_HTTP_NO_AUTH=1` is set, so a chart that rendered it without either
would deploy a crash-looping pod.

## Decision

We will build `vmafx-mcp` in the go-builder stage of `docker/Dockerfile.node`,
add it to that stage's licence inventory and copy it into every node target
(CPU, CUDA, HIP, SYCL). The chart gains an `mcp` component, off by default:
a Deployment and Service `<fullname>-mcp` with component label `mcp`, run from
the node image with the command `vmafx-mcp` unless `mcp.image.repository`
names another image. Its environment is generated from
`api/vmafx-platform.toml` like the other workloads: streamable HTTP on
`mcp.httpPort` (3000), the controller address of the chart's controller, the
token from `mcp.auth.token` and the controller token from
`mcp.controllerToken`. The chart refuses `mcp.enabled` without
`mcp.auth.token.secretName` unless `mcp.auth.noAuth` is true. With
`networkPolicy.enabled` it renders `allow-mcp-ingress`,
`allow-mcp-to-controller` and `allow-controller-from-mcp`. `vmafx-mcp
--version` prints the build version and exits, so an image can be checked
without a controller.

Node GPU selection on Intel xe stays the device plugin resource
`gpu.intel.com/xe` (`gpu.intelDriver: xe`, ADR-1547); the chart names no
device file, host path or node. `scripts/ci/tests/test_helm_mcp.py` holds
both.

## Alternatives considered

| Option | For | Against |
| --- | --- | --- |
| Binary in the node image (chosen) | one image, one licence inventory, every node variant runs MCP | the MCP pod pulls a larger image than it needs |
| Separate `vmafx-mcp` image | smallest pod | a new publish job, signing and licence inventory for one binary |
| Render MCP without auth by default | simplest install | a pod that refuses to start, or an open endpoint on the cluster network |

## Consequences

- An install enables MCP with `--set mcp.enabled=true` and a token Secret; a
  missing token fails the render, not the pod.
- Every node image grows by the `vmafx-mcp` binary.
- New NetworkPolicy allow keys `mcpIngress` and `mcpToController`.

## References

- Operator order vmafx-chart-mcp-component-20261024 (paraphrased): add an MCP
  component with Deployment, Service, values block off by default, auth and
  network policy, and Intel xe node selection through the device plugin
  resource.
- Ledger answer vmafx-chart-mcp-image, option a (paraphrased): build the
  binary into the node image and default the chart's MCP image to it.
- [ADR-1547](1547-helm-gpu-resource-name.md), [ADR-0930](0930-helm-networkpolicy-pss.md).
