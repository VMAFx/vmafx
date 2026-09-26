<!-- markdownlint-disable MD013 -->
# Historical snapshot: Zed migration refresh (2026-05-22)

> **Archived, not operational guidance.** This page recorded the Zed 1.3.6
> experiment merged by `3f4f28a5a`. Use the current
> [IDE setup guide](ide-setup.md) and checked-in `.zed/` files instead.

The original refresh explored project tasks, debugger scenarios, ACP registry
agents, MCP integration, and edit-prediction exclusions. Commit `196a572ab`
then silently replaced the three `.zed/` files with older copies while changing
unrelated cJSON code. BUG-048's 2026-09-24 audit restored only the portions that
remain valid on Zed 1.18.1.

The 1.3.6 configuration is unsafe to copy today:

- `agent` and `agent_servers` are user-setting roots, not fields accepted by
  `.zed/settings.json`'s `ProjectSettingsContent` parser;
- external ACP agents now own their authentication and model selection, so a
  project must not pin old Claude model IDs;
- custom MCP servers use `command` plus `args`, without `source: "custom"`;
- the current container MCP executable is the Go `vmafx-mcp` binary from
  ADR-1229, not the deprecated Python `vmaf-mcp` entrypoint;
- the repository no longer promises `.venv/bin/ruff`, `.venv/bin/shfmt`,
  deleted `scripts/dev/` helpers, or the retired numbered workspace root;
- the vmaf-tune package lives under `tools/vmaf-tune/src/vmaftune` and its
  public compare command consumes `--src`, not the removed direct script path;
  and
- Vulkan was removed by ADR-0726, so its GLSL-specific setup is not a current
  VMAFx requirement.

Historical design evidence remains in
[`docs/research/0729-zed-config-1-3-6-refresh.md`](../research/0729-zed-config-1-3-6-refresh.md)
and in Git history at `3f4f28a5a`. The current audit and alternatives are in
[`docs/research/bug048-zed-1-18-restoration-2026-09-24.md`](../research/bug048-zed-1-18-restoration-2026-09-24.md).
