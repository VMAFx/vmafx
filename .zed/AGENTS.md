<!-- markdownlint-disable MD013 -->
# `.zed/` project-configuration invariants

Parent: [../AGENTS.md](../AGENTS.md).

Zed reads `.zed/settings.json` through `ProjectSettingsContent`, not through
the full user-settings schema. Keep `agent`, `agent_servers`, extension
installation, UI preferences, telemetry, provider/model selection, and agent
permission policy out of this directory. External ACP agents own their own
authentication, model selection, and write/approval mode.

The checked-in MCP context server launches the current Go executable as
`docker exec -i vmaf-dev-mcp vmafx-mcp`. Do not restore the deprecated Python
`vmaf-mcp` entrypoint or the old `source: "custom"` field. Tasks and debugger
scenarios must use current tracked entrypoints, the writable container
`/probes` area, and the active `.workingdir/` root; never restore deleted
helpers, `.venv/bin/*` assumptions, or the retired numbered workspace root.

Preserve the three `Standards:` governance tasks. After any change under this
directory, run:

```bash
python3 -m pytest -q scripts/ci/tests/test_zed_project_config.py
```
