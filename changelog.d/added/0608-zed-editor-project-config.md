- **`.zed/` project configuration** — Zed editor parity with `.vscode/` (ADR-0608):
  `.zed/settings.json` (clangd LSP, pyright, ruff, shfmt, vmaf-mcp MCP server,
  Claude Agent via ACP, file-type associations for CUDA/HIP/Metal/GLSL, telemetry
  disabled), `.zed/tasks.json` (15 tasks mirroring all Makefile targets + meson
  invocations + dev-mcp shell + MCP smoke test), `.zed/debug.json` (CodeLLDB
  configurations for vmaf CLI and C unit tests).
- **`docs/development/ide-setup.md`** — Zed section documenting minimum version
  (1.2.6+), required extensions (clangd, ShellCheck, CodeLLDB, GLSL), known gaps
  (Meson problem matcher, Nsight, PR UI), ACP/memory continuity guarantee, and
  MCP server first-time setup.
- **`.gitignore`** — `.zed/local/` excluded (Zed's per-user auth tokens and
  machine-local state); `.zed/` itself is committed as shared project config.
