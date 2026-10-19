---
paths:
  - .zed/*
  - scripts/ci/tests/test_zed_project_config.py
invariant: `.zed/` holds project-scoped settings only; test and files change together, on exact installed-version proof.
area: gates
---
<!-- markdownlint-disable MD013 MD060 -->
# Zed project-configuration contract

`tests/test_zed_project_config.py` is fail-closed contract for
project-scoped Zed files. It must keep rejecting user-only `agent` and
`agent_servers` roots, retired numbered workspace root, `.venv/bin/`
assumptions, deleted helper paths, deprecated Python MCP entrypoint, and
loss of three standards-governance tasks. Update test together with
`.zed/` only when exact installed-version source proves schema or executable
change; Zed JSON parse alone does not prove that project settings apply
keys.
