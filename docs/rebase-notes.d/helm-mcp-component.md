## Helm chart MCP component (2026-10-24)

- `deploy/helm/vmafx/values.yaml`, `values.schema.json` and
  `templates/_config.gen.tpl` stay generated from `api/vmafx-platform.toml`;
  on a conflict take either side and run
  `python3 scripts/codegen/vmafx-api.py --write`.
- `templates/mcp-validate.yaml` refuses `mcp.enabled` without a token Secret
  or `mcp.auth.noAuth`; keep it with `scripts/ci/tests/test_helm_mcp.py`.
- `docker/Dockerfile.node` builds `vmafx-mcp` in the go-builder stage and every
  node target copies it; the licence inventory lists it.
