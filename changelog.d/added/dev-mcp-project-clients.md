- **Coding-agent clients reach the dev container's MCP server.** The project
  files `.mcp.json`, `opencode.json` and `.gemini/settings.json` wire
  `vmafx-dev` (`docker exec -i vmaf-dev-mcp vmafx-mcp`), so Claude Code,
  opencode, Gemini CLI and Antigravity can score, probe and benchmark through
  the container; see the dev-MCP guide for Codex and the start step.
