## Dev MCP server in the project client files (2026-10-28)

- `.mcp.json`, `opencode.json` and `.gemini/settings.json` carry a
  `vmafx-dev` entry next to Praetor's `asdecided`. `praetorctl adopt` merges
  its own server into these files and keeps other entries; a sync or adopt
  run that rewrites them keeps `vmafx-dev`.
