<!-- markdownlint-disable MD013 MD060 -->
# ADR-0608: Commit `.zed/` project configuration for Zed editor parity with `.vscode/`

- **Status**: Accepted
- **Date**: 2026-05-19
- **Deciders**: lusoris
- **Tags**: `dev`, `ide`, `docs`, `build`, `workspace`

## Context

The fork ships `.vscode/` (settings, tasks, launch configs, and extensions list) so
operators can open the repo in VS Code and get working clangd LSP, format-on-save,
debug configurations, and task shortcuts with zero manual setup. As of 2026-05-19
the team migrated primary editor usage to Zed 1.2.6 (released 2026-05-15).

Zed reads a `.zed/` directory for project-local settings. Without this directory,
every new contributor must manually configure clangd arguments, MCP servers, LSP
formatters, and tasks — repeating the same setup that `.vscode/` eliminated for VS
Code users. The migration plan (`docs/development/zed-migration-plan-2026-05-19.md`)
audited Zed's feature set against this repo's requirements and confirmed that:

- clangd, pyright, and ruff LSPs work via `.zed/settings.json` `lsp` key.
- The vmaf-mcp stdio transport is natively supported via `context_servers`.
- Claude Agent (Claude Code CLI) runs via ACP and requires no editor-level config.
- All six `.claude/hooks/` scripts fire normally inside the ACP agent process.
- `.vscode/` and `.zed/` coexist without conflict.

Sources:

- <https://zed.dev/docs/reference/all-settings>, retrieved 2026-05-19
- <https://zed.dev/docs/configuring-languages>, retrieved 2026-05-19
- <https://zed.dev/docs/ai/mcp>, retrieved 2026-05-19
- <https://zed.dev/docs/ai/external-agents>, retrieved 2026-05-19
- <https://zed.dev/docs/tasks>, retrieved 2026-05-19
- <https://zed.dev/docs/debugger>, retrieved 2026-05-19

## Decision

Commit a `.zed/` directory at the repo root with:

1. **`.zed/settings.json`** — clangd LSP (same arguments as `.vscode/settings.json`),
   pyright + ruff for Python, shfmt for shell, format-on-save per language, vmaf-mcp
   registered as a `context_servers` entry with `source: "custom"`, Claude Agent
   registered implicitly via ACP, file-type associations for `.cu`/`.cuh`/`.hip`/`.metal`
   and GLSL shader extensions, telemetry disabled.
2. **`.zed/tasks.json`** — full task list mirroring all `Makefile` targets and key
   `meson` invocations, plus a dev-mcp shell task and MCP smoke test.
3. **`.zed/debug.json`** — CodeLLDB-based debug configurations for the vmaf CLI and
   `test_feature` C unit test binary, replacing the GDB adapter that Zed does not
   natively support.

`.zed/local/` is added to `.gitignore` to exclude Zed's per-user machine-local state.
`.vscode/` is not modified. `docs/development/ide-setup.md` is updated with a Zed
section documenting required extensions, known gaps, and the ACP/memory continuity
guarantee.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Document Zed setup in a README only; no committed config | Zero files to maintain | Every new contributor repeats the same manual setup; no canonical clangd args, no MCP entry | Inconsistent with the `.vscode/` precedent; violates the "no docs without committed config" principle for user-discoverable surfaces |
| Symlink or merge `CLAUDE.md` into root `AGENTS.md` | Zed's agent panel sees full rules | `AGENTS.md` already exists and mirrors `CLAUDE.md` content for tool-agnostic agents; duplication creates drift risk | Overkill — Zed's first-party panel picks `AGENTS.md` (position 7); Claude Agent reads `CLAUDE.md` directly via ACP regardless |
| Create a `.rules` file (position 1 in Zed's priority list) | Highest-priority rules file for Zed's agent panel | Another file to maintain; Claude Code CLI does not read `.rules`; adds confusion | Not enough value to justify a third rules file |
| Port the entire `.vscode/launch.json` to `.zed/debug.json` with GDB | Familiar adapter | Zed's DAP list does not include GDB as a built-in; CodeLLDB is the recommended C/C++ path | Use CodeLLDB instead — cross-platform, actively maintained, listed in Zed docs |

## Consequences

- **Positive**: New contributors using Zed get working clangd, formatters, MCP, Claude
  Agent, and tasks with no manual configuration. The repo is editor-agnostic at the
  project-config level.
- **Positive**: vmaf-mcp tools (`vmaf_score`, `list_models`, `list_backends`) are
  available in Zed agent threads automatically via ACP forwarding.
- **Negative**: A second set of IDE config files to keep in sync with `.vscode/`.
  Mitigated by the overlap being small (clangd args, task commands) and both sets
  remaining in-tree so drift is visible in PRs.
- **Neutral**: `.zed/debug.json` supersedes `.vscode/launch.json` for Zed users.
  VS Code users are unaffected.
- **Neutral / follow-up**: The CodeLLDB Zed debugger extension must be installed
  manually (not auto-installed). Document in `docs/development/ide-setup.md` (done
  in this PR).

## References

- Migration plan: `docs/development/zed-migration-plan-2026-05-19.md`
- <https://zed.dev/docs/reference/all-settings>, retrieved 2026-05-19
- <https://zed.dev/docs/ai/mcp>, retrieved 2026-05-19
- <https://zed.dev/docs/ai/external-agents>, retrieved 2026-05-19
- <https://zed.dev/docs/debugger>, retrieved 2026-05-19
- <https://markaicode.com/mcp-zed-editor-setup/>, retrieved 2026-05-19 (`source: "custom"` requirement)
- req: "COMMIT that skeleton + everything needed to actually use Zed on this repo. No half measures — the user explicitly said everything that makes life better/faster/easier/cheaper."
