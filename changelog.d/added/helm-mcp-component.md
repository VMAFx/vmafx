- **Helm chart MCP component (ADR-3369).** `mcp.enabled=true` deploys the
  Go MCP server `vmafx-mcp` as a Deployment and Service from the node image
  (every node image now ships the binary), connected to the chart's
  controller. The chart refuses the setting without a bearer token Secret
  (`mcp.auth.token.secretName`) unless `mcp.auth.noAuth=true` is set on
  purpose, and renders NetworkPolicies for MCP ingress and MCP to
  controller. `vmafx-mcp --version` prints the build version. See
  [the Kubernetes deployment guide](docs/development/k8s-deployment.md#mcp-server).
