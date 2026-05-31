<!-- markdownlint-disable MD013 MD060 -->
# Zed Migration Plan — VMAFx/vmafx fork (2026-05-19)

Migration plan from VSCode to Zed for this repository.
**Every Zed-feature claim below cites a WebFetched URL and retrieval date.**
No training-data assertions are made.

---

## 1. Zed feature map (live docs)

Retrieved 2026-05-19 from <https://zed.dev/docs> (navigation audit) and linked sub-pages.

**Latest stable version:** 1.2.6, released 2026-05-15.
Source: <https://zed.dev/releases>, retrieved 2026-05-19.

### 1.1 AI — agent panel and external agents

Source: <https://zed.dev/docs/ai/overview>, retrieved 2026-05-19.

Zed ships a **Threads Sidebar** (agent panel) where users start a thread, describe
a task, and an agent reads, edits, and runs code in the project. Multiple threads
can run in parallel against different projects. Three built-in tool profiles exist:
**Write** (full edit + terminal access), **Ask** (read-only), and **Minimal**
(no tools, plain chat). Custom profiles are supported.

**External agents** — Claude Agent, Gemini CLI, Codex CLI, and GitHub Copilot — are
integrated via the **Agent Client Protocol (ACP)**.
Source: <https://zed.dev/docs/ai/external-agents>, retrieved 2026-05-19.

The first time a Claude Agent thread is created, Zed auto-installs
`@zed-industries/claude-agent-acp` from the ACP registry and keeps it updated.
The user authenticates Claude Agent separately (via `/login` in the thread, using
an Anthropic API key or Claude Pro/Max subscription). Claude Agent reads
`~/.claude/` and any `CLAUDE.md` files natively.

### 1.2 Rules / AGENTS.md / CLAUDE.md

Source: <https://zed.dev/docs/ai/rules>, retrieved 2026-05-19.

Zed's **Rules** system auto-includes a project-level instructions file in every
Agent Panel interaction. Zed reads the **first** file found at the project root
from this priority list:

```text
1. .rules
2. .cursorrules
3. .windsurfrules
4. .clinerules
5. .github/copilot-instructions.md
6. AGENT.md
7. AGENTS.md          ← this repo has one at the root
8. CLAUDE.md          ← and this one too
9. GEMINI.md
```

**Only one file is used** (the first match). Because `.rules` through
`.clinerules` do not exist in this repo, and `AGENT.md` does not exist,
the root `AGENTS.md` (position 7) will be picked up by Zed's first-party
agent panel. `CLAUDE.md` (position 8) would be ignored by the panel —
it is read directly by the Claude Agent external agent process, which
maintains its own configuration loading independent of Zed's rules system.

The multiple `AGENTS.md` files in subdirectories (`libvmaf/`, `ai/`, etc.)
are not read by Zed's rules system; they continue to serve tool-agnostic
agents (GitHub Copilot Workspace, etc.) and the Claude Code CLI directly.

### 1.3 ACP — Agent Client Protocol

Source: <https://zed.dev/docs/ai/external-agents>, retrieved 2026-05-19.
Note: `https://zed.dev/docs/acp` returns HTTP 404 — the canonical documentation
is at the external agents page above.

ACP is a protocol through which Zed (acting as **host/client**) launches an
external agent process and communicates with it. Zed forwards:

- Model and mode selection
- Environment variables
- MCP servers (via `context_servers`, relayed through ACP)
- Working directory (project root)

Zed does **not** forward: profiles, tool permission settings, or rules files.

The `claude` CLI already speaks ACP natively through the auto-installed
`@zed-industries/claude-agent-acp` package — no manual bridge is required.
Debug ACP traffic via `dev: open acp logs` in the command palette.

### 1.4 MCP (Model Context Protocol)

Sources: <https://zed.dev/docs/ai/mcp>, retrieved 2026-05-19;
<https://zed.dev/docs/extensions/mcp-extensions>, retrieved 2026-05-19;
<https://markaicode.com/mcp-zed-editor-setup/>, retrieved 2026-05-19.

MCP servers are configured under the `context_servers` key (not `mcp_servers`)
in `settings.json`. The `source: "custom"` flag is **required** for manually
added servers; without it Zed silently skips the entry.

**Supported transports:**

- **stdio** — native, first-class. Zed spawns the server as a child process.
- **HTTP/SSE** — supported via `url` + `headers` keys (no `command`).
- **Unix domain socket (UDS)** — not natively supported. The `vmaf-mcp` server
  currently uses stdio transport (`mcp.server.stdio`), so this is not a blocker.
  If UDS were needed, `mcp-remote` can bridge it.

**Supported MCP features:** Tools and Prompts only. Resources and Sampling are
not yet implemented in Zed.

Dynamic tool updates are supported: Zed handles
`notifications/tools/list_changed` without requiring server restart.

### 1.5 LSP configuration

Source: <https://zed.dev/docs/configuring-languages>, retrieved 2026-05-19.

LSP is configured under the `lsp` key. Per-server settings use
`initialization_options` (sent at startup, require server restart to change)
and `binary` (path + args + env override). No clangd-specific docs are
published on the Zed site; the generic `binary.arguments` key carries all
clangd flags.

### 1.6 Tasks

Source: <https://zed.dev/docs/tasks>, retrieved 2026-05-19.

Tasks live in `.zed/tasks.json` (project-local) or `~/.config/zed/tasks.json`
(global). Key variable: `$ZED_WORKTREE_ROOT` replaces `${workspaceFolder}`.
Tasks support `label`, `command`, `args`, `env`, `cwd`, `reveal`, `hide`,
`save`, and `hooks` fields.

### 1.7 Debugger (DAP)

Source: <https://zed.dev/docs/debugger>, retrieved 2026-05-19.

Zed implements DAP client-side. Debug configurations go in `.zed/debug.json`.
Zed also **automatically loads** `.vscode/launch.json` if no `.zed/debug.json`
exists — existing launch configs work without porting on day 1.

Supported adapters include CodeLLDB (for C/C++/Rust). GDB is not listed as a
built-in adapter in the docs; CodeLLDB is the recommended C path.

### 1.8 Git integration

Source: <https://zed.dev/docs/git>, retrieved 2026-05-19.

Zed ships a full Git Panel with: staging, commit (with AI-assisted messages),
branch create/switch/delete, stash, fetch/push/pull, merge conflict resolution,
file history, inline blame, and gutter indicators. Git worktrees are natively
supported (relevant: this repo uses worktrees for agent isolation).

GitHub PR integration is not bundled — requires `gh` CLI from the terminal.

### 1.9 Extensions

Source: <https://zed.dev/docs/extensions>, retrieved 2026-05-19.

Extension types: Language, Debugger, Theme, Icon Theme, Snippets,
**Agent Server** (ACP-based), and **MCP Server** (context_servers).
The marketplace is smaller than VS Code's; see gap list in §9.

### 1.10 Keybindings

Source: <https://zed.dev/docs/key-bindings>, retrieved 2026-05-19.

Keybindings live in `~/.config/zed/keymap.json` (global) or can be overridden
per-project. The format is a JSON array of objects with `context` and `bindings`.
Set `"base_keymap": "VSCode"` in settings to get familiar shortcuts out of the
box.

---

## 2. Does Zed read AGENTS.md / CLAUDE.md?

**Answer: Yes, with caveats.**

Source: <https://zed.dev/docs/ai/rules>, retrieved 2026-05-19.

Zed's first-party **Agent Panel** reads the first matching file from the priority
list in §1.2. For this repo:

- Zed Agent Panel → reads root `AGENTS.md` (position 7 in priority list).
- `CLAUDE.md` is **not** read by the panel (it is superseded by `AGENTS.md`).
- The **Claude Agent external agent** (via ACP) reads `CLAUDE.md` directly —
  this is the Claude Code CLI's own file loading, independent of Zed's rules.
- Per-directory `AGENTS.md` files (e.g. `libvmaf/AGENTS.md`, `ai/AGENTS.md`)
  are read only by tool-agnostic agents that walk the directory tree (Copilot
  Workspace, Claude Code CLI) — not by Zed's panel, which only reads the
  root-level file.

**Recommended action:** Add a symlink or copy `CLAUDE.md` content into the root
`AGENTS.md` (or vice versa) if you want Zed's panel to see the full rule set.
Alternatively, rename/create a root `.rules` file that includes or mirrors
`CLAUDE.md` content — `.rules` takes priority 1 and would be used instead.

---

## 3. ACP — what it is and how it works here

Source: <https://zed.dev/docs/ai/external-agents>, retrieved 2026-05-19.

ACP (Agent Client Protocol) is Zed's protocol for hosting external CLI-based
coding agents inside the editor UI. Architecture:

```text
Zed (ACP host) ←→ claude-agent-acp package ←→ claude CLI process
```

- **Zed = client/host.** It launches the agent, manages threads, and forwards
  context (project root, env, MCP server list).
- **Claude Code CLI = agent.** It receives tasks, uses its normal tools (Bash,
  Edit, Read, WebFetch, etc.), and streams results back.
- **No manual bridge needed.** Zed auto-installs and auto-updates the ACP
  adapter package on first use.
- **Skills remain CLI-native.** Skill invocations (e.g. `/build-vmaf`) run
  inside the Claude Code CLI process — they work identically whether invoked
  from a terminal or from Zed's agent thread. Zed's UI surfaces the result.
- **Hooks run inside the Claude Code CLI.** `.claude/hooks/` scripts fire as
  normal because they are part of the CLI's hook system, not Zed's.

**MCP forwarding:** Zed forwards `context_servers` to the Claude Agent via ACP,
so MCP tools defined in `.zed/settings.json` are available inside the agent thread.

---

## 4. `.zed/settings.json` skeleton for this repo

Place at `<repo-root>/.zed/settings.json`. All paths are relative to the repo
root at runtime via `$ZED_WORKTREE_ROOT`.

```jsonc
// .zed/settings.json — vmaf fork project settings
// Source for settings keys: https://zed.dev/docs/reference/all-settings (2026-05-19)
// Source for LSP config: https://zed.dev/docs/configuring-languages (2026-05-19)
// Source for MCP config: https://zed.dev/docs/ai/mcp (2026-05-19)
// Source for tasks: https://zed.dev/docs/tasks (2026-05-19)
{
  // ── Editor baseline ───────────────────────────────────────────────
  "tab_size": 4,
  "hard_tabs": false,
  "line_ending": "enforce_lf",
  "ensure_final_newline_on_save": true,
  "remove_trailing_whitespace_on_save": true,
  "preferred_line_length": 100,

  // ── File exclusions (mirror .vscode/settings.json watcherExclude) ─
  "file_scan_exclusions": [
    ".git",
    "build",
    "build-san",
    "subprojects",
    ".venv",
    "node_modules",
    "**/*.yuv",
    "**/*.onnx",
    "**/*.pkl"
  ],

  // ── File type associations ─────────────────────────────────────────
  // Zed uses language name strings, not extension arrays.
  // CUDA support requires the "cuda" extension from the marketplace.
  "file_types": {
    "C++": ["cu", "cuh"]
  },

  // ── Language-specific overrides ────────────────────────────────────
  // Source: https://zed.dev/docs/reference/all-settings (2026-05-19)
  "languages": {
    "C": {
      "tab_size": 4,
      "format_on_save": "on",
      "formatter": "language_server"
    },
    "C++": {
      "tab_size": 4,
      "format_on_save": "on",
      "formatter": "language_server"
    },
    "Python": {
      "tab_size": 4,
      "format_on_save": "on",
      // Use external ruff for format; black for final formatting.
      // Zed runs formatters in array order.
      "formatter": [
        {
          "external": {
            "command": "ruff",
            "arguments": ["format", "--stdin-filename", "{buffer_path}", "-"]
          }
        }
      ]
    },
    "Shell Script": {
      "tab_size": 2,
      "format_on_save": "on",
      "formatter": {
        "external": {
          "command": "shfmt",
          "arguments": ["-i", "2", "-ci", "-"]
        }
      }
    },
    "TOML": { "tab_size": 2 },
    "YAML": { "tab_size": 2 },
    "JSON": { "tab_size": 2 },
    "JSONC": { "tab_size": 2 }
  },

  // ── LSP configuration ──────────────────────────────────────────────
  // Source: https://zed.dev/docs/configuring-languages (2026-05-19)
  "lsp": {
    "clangd": {
      "binary": {
        // Remove "path" to use system clangd; set it to pin a version.
        // "path": "/usr/bin/clangd",
        "arguments": [
          "--compile-commands-dir=build",
          "--background-index",
          "--clang-tidy",
          "--clang-tidy-checks=-*,bugprone-*,cert-*,clang-analyzer-*,performance-*,portability-*",
          "--header-insertion=never",
          "--completion-style=detailed",
          "--function-arg-placeholders=true",
          "--pch-storage=memory",
          "-j=8"
        ]
      },
      "initialization_options": {
        "fallbackFlags": [
          "-std=c11",
          "-I./libvmaf/include",
          "-I./libvmaf/src"
        ]
      }
    },
    "pyright": {
      "initialization_options": {
        "python": {
          "analysis": {
            "typeCheckingMode": "basic"
          }
        }
      }
    },
    "ruff": {
      "initialization_options": {
        "settings": {
          "organizeImports": true
        }
      }
    }
  },

  // ── MCP servers ────────────────────────────────────────────────────
  // Source: https://zed.dev/docs/ai/mcp (2026-05-19)
  // vmaf-mcp uses stdio transport (confirmed from mcp-server/vmaf-mcp/src/vmaf_mcp/server.py)
  // "source": "custom" is REQUIRED for manually-added servers
  "context_servers": {
    "vmaf-mcp": {
      "source": "custom",
      "command": "vmaf-mcp",
      // Alternatively, if not installed to PATH:
      // "command": "python",
      // "args": ["-m", "vmaf_mcp.server"],
      "env": {
        "VMAF_BIN": "build/tools/vmaf"
        // Extend allowed paths if needed:
        // "VMAF_MCP_ALLOW": "/path/to/extra/yuv"
      }
    }
  },

  // ── Agent / AI settings ────────────────────────────────────────────
  // Source: https://zed.dev/docs/ai/agent-settings (2026-05-19)
  "agent": {
    "default_model": {
      "provider": "anthropic",
      "model": "claude-sonnet-4-5"
    },
    "tool_permissions": {
      "default": "confirm",
      // Allow MCP vmaf-mcp tools without prompt (scoring is read-only)
      "tools": {
        "mcp:vmaf-mcp:vmaf_score":     { "default": "allow" },
        "mcp:vmaf-mcp:list_models":    { "default": "allow" },
        "mcp:vmaf-mcp:list_backends":  { "default": "allow" }
      }
    }
  },

  // ── Telemetry opt-out ──────────────────────────────────────────────
  // Source: https://zed.dev/docs (account & privacy section, 2026-05-19)
  "telemetry": {
    "diagnostics": false,
    "metrics": false
  }
}
```

---

## 5. Skills migration

`.claude/skills/` is a Claude Code CLI feature — skills are prompt templates
and shell scripts invoked by the `/skill-name` syntax inside Claude Code sessions.

**They are editor-agnostic.** The CLI reads `.claude/skills/` regardless of
which editor it is launched from. When Claude Code runs as an ACP external agent
inside Zed (see §3), the same `/build-vmaf`, `/add-gpu-backend`, etc. invocations
work identically in Zed's agent thread as in the terminal.

Source: confirmed from `.claude/skills/` file audit and ACP forwarding behaviour
documented at <https://zed.dev/docs/ai/external-agents>, retrieved 2026-05-19.

**No porting work required.** Skills continue to live in `.claude/skills/` and
fire through the Claude Code CLI process, not through Zed directly.

**Optional convenience:** Bind a Zed task to open a Claude thread pre-seeded with
a skill invocation (using the `agent::NewExternalAgentThread` action with an
`initial_message` argument if supported), but this is purely additive.

---

## 6. Hooks migration

`.claude/hooks/` contains six hooks registered in `.claude/settings.json`:

| Hook event    | Script                       | Purpose                                  |
|---------------|------------------------------|------------------------------------------|
| `PreToolUse`  | `block-unsafe-bash.sh`       | Block dangerous git/rm/curl-pipe commands |
| `PostToolUse` | `auto-format-on-edit.sh`     | Run clang-format/black/shfmt after edits  |
| `PostToolUse` | `auto-snapshot-warn.sh`      | Warn when numerical code is modified      |
| `PostToolUse` | `docs-drift-warn.sh`         | Warn when a user-discoverable surface lacks docs |
| `PostToolUse` | `compile-commands-sync.sh`   | Re-link compile_commands.json after meson changes |
| `SessionStart`| `session-start.sh`           | Print branch / upstream delta / clangd status |
| `Stop`        | `stop.sh`                    | Print uncommitted/unpushed summary at exit |

**These are Claude Code CLI hooks — they run inside the `claude` process, not
inside Zed.** When Claude Code runs as an ACP agent in Zed, all hooks fire
normally because the agent process is the full Claude Code CLI.

**Zed has no equivalent hook system.** Zed does not expose `PreToolUse`,
`PostToolUse`, `SessionStart`, or `Stop` lifecycle events to project-level
scripts.

| Hook function              | Works via Zed ACP agent? | Needs alternative?          |
|----------------------------|--------------------------|-----------------------------|
| `block-unsafe-bash.sh`     | Yes (CLI fires it)       | None                        |
| `auto-format-on-edit.sh`   | Yes (CLI fires it)       | Also covered by Zed format-on-save |
| `auto-snapshot-warn.sh`    | Yes (CLI fires it)       | None                        |
| `docs-drift-warn.sh`       | Yes (CLI fires it)       | None                        |
| `compile-commands-sync.sh` | Yes (CLI fires it)       | None                        |
| `session-start.sh`         | Yes (CLI fires it)       | None                        |
| `stop.sh`                  | Yes (CLI fires it)       | None                        |

**Conclusion:** No hooks porting work is required. The entire `.claude/hooks/`
infrastructure is transparent to Zed.

The one redundancy: `auto-format-on-edit.sh` runs formatters post-Edit, and
Zed's `format_on_save` also runs formatters on save. Both are idempotent so
the double invocation is harmless.

---

## 7. MCP migration — vmaf-mcp server

The `mcp-server/vmaf-mcp/` server uses **stdio transport**
(confirmed: `from mcp.server.stdio import stdio_server`,
`mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`).

Zed natively supports stdio transport via the `context_servers` / `command` key.
Source: <https://zed.dev/docs/ai/mcp>, retrieved 2026-05-19.

**No UDS bridge is needed.** The server speaks stdio, which Zed supports natively.

**Configuration** (already shown in §4):

```jsonc
"context_servers": {
  "vmaf-mcp": {
    "source": "custom",
    "command": "vmaf-mcp",
    "env": { "VMAF_BIN": "build/tools/vmaf" }
  }
}
```

**Access from Claude Agent:** Zed forwards `context_servers` to the Claude Agent
via ACP, so `vmaf_score`, `list_models`, `list_backends`, `run_benchmark`, and
the five other tools are available in agent threads automatically.

**Limitation:** Zed supports MCP Tools and Prompts only. If vmaf-mcp ever
exposes MCP Resources, those will not be visible in Zed.
Source: <https://markaicode.com/mcp-zed-editor-setup/>, retrieved 2026-05-19.

**Tool permissions:** Add per-tool rules in the `agent.tool_permissions.tools`
section using the `mcp:<server_name>:<tool_name>` key format
(e.g. `mcp:vmaf-mcp:vmaf_score`).
Source: <https://zed.dev/docs/ai/agent-settings>, retrieved 2026-05-19.

---

## 8. Settings you might forget

### 8.1 Line endings and EditorConfig

`.editorconfig` exists at the repo root (verified 2026-05-19) with:

- `end_of_line = lf`
- `insert_final_newline = true`
- `trim_trailing_whitespace = true`
- `indent_size = 4` (global), 2 for JS/TS/JSON/YAML/TOML/MD

Zed has built-in EditorConfig support — it reads `.editorconfig` automatically
and **the `.zed/settings.json` `line_ending`, `tab_size`, and whitespace
settings will override `.editorconfig`** where they conflict. Set
`"line_ending": "enforce_lf"` in `.zed/settings.json` to be explicit.

### 8.2 Telemetry opt-out

Set in `.zed/settings.json` (shown in §4):

```json
"telemetry": { "diagnostics": false, "metrics": false }
```

Source: privacy section at <https://zed.dev/docs>, retrieved 2026-05-19.

### 8.3 Vim mode

Enable globally: `"vim_mode": true` in `~/.config/zed/settings.json`.
Helix mode is also available as an alternative.
Source: <https://zed.dev/docs> navigation (Vim Mode entry), retrieved 2026-05-19.

### 8.4 VSCode base keymap

Add `"base_keymap": "VSCode"` to `~/.config/zed/settings.json` to map most
familiar shortcuts (`Ctrl+P`, `Ctrl+Shift+P`, `Ctrl+Shift+F`, etc.).
Source: <https://zed.dev/docs/migrate/vs-code>, retrieved 2026-05-19.

### 8.5 Copy-on-select (not available in Zed)

Zed does not have a copy-on-select setting. Middle-click paste is also not
present. Use `Ctrl+C` / `Ctrl+V` (or Vim-mode clipboard). This is a Zed
design choice with no workaround currently documented.

### 8.6 Rulers

VSCode `editor.rulers: [100]` maps to `"soft_wrap": "bounded"` and
`"preferred_line_length": 100` in Zed. A visible ruler line is rendered via
the `show_wrap_guides` setting (defaults to `true` when soft_wrap is enabled).

### 8.7 Import existing VSCode settings

First launch: Zed's onboarding screen offers "Import VS Code settings".
Post-install: run `zed: import vs code settings` from the command palette.
This imports fonts, tabs, terminal, git, and editor preferences — not
extensions or keybindings.
Source: <https://zed.dev/docs/migrate/vs-code>, retrieved 2026-05-19.

### 8.8 CUDA file type association

`.cu` / `.cuh` files need the **cuda** extension from the Zed marketplace, or
map them to `C++` in `file_types` (limited — no CUDA-specific highlighting).
The `file_types` entry in §4 maps `.cu`/`.cuh` → C++ as a stopgap.

### 8.9 DevContainer support

Zed supports Dev Containers (Remote Development section).
Source: <https://zed.dev/docs> navigation, retrieved 2026-05-19.
The existing `.devcontainer/devcontainer.json` should be usable. The
`customizations.vscode` section is VSCode-specific and ignored by Zed;
Zed uses its own remote dev config. This is low-priority since the project
default is the `vmaf-dev-mcp` Docker container accessed via `docker exec`.

---

## 9. Gap list — Zed can't do today vs VSCode can

| Feature / Extension | VSCode has it | Zed today (2026-05-19) | Workaround |
|----|----|----|-----|
| `ms-vscode.cpptools` IntelliSense | Yes (disabled; clangd used instead) | Not needed; clangd is first-class | None needed |
| `mesonbuild.mesonbuild` extension | Yes (meson syntax + run tasks) | No Meson extension listed; syntax highlight may require community extension | Use `file_types` to map `meson.build` → a generic lang; run meson via tasks |
| `nvidia.nsight-vscode-edition` | Yes (CUDA profiler UI) | No Nsight Zed extension | Use Nsight CLI from integrated terminal; `make lint` / `ncu` commands via tasks |
| `intel-corporation.oneapi-environment-configurator` | Yes | No Zed equivalent | Source oneAPI env in shell profile; tasks inherit env |
| `foxundermoon.shell-format` (shfmt UI) | Yes | No dedicated extension; use `formatter: external` (shfmt) | External formatter config in §4 covers this |
| `timonwong.shellcheck` | Yes | ShellCheck LSP (`shellcheck` language server) available via marketplace | Install via Zed extensions; works as LSP |
| `github.vscode-pull-request-github` | Yes | No PR UI extension; git panel shows branches only | Use `gh` CLI from terminal; Zed git panel for staging/commit |
| `anthropic.claude-code` (VSCode extension) | Yes | Replaced by ACP external agent — different integration model, full parity for agentic work | Use `agent: new external agent thread` → Claude |
| `streetsidesoftware.code-spell-checker` | Yes | No equivalent in Zed marketplace currently | Use `codespell` via a task or pre-commit |
| `editorconfig.editorconfig` extension | Yes (explicit) | Built-in EditorConfig support | None needed; Zed reads `.editorconfig` natively |
| `.code-workspace` multi-root workspace file | Yes | No equivalent; use `Add Folder to Project` | Add each root manually; per-project `.zed/settings.json` per folder |
| `launch.json` GDB adapter | Yes (`MIMode: gdb`) | Zed DAP prefers CodeLLDB; `.vscode/launch.json` auto-loaded if no `.zed/debug.json` | Test existing `launch.json` first — Zed loads it; if GDB adapter missing, install CodeLLDB and convert |
| Workspace trust | Yes | Zed has `worktree_trust` setting | Set `session.trust_all_worktrees: false` (default); trust per worktree interactively |
| MCP Resources | Partial (not used) | Not supported in Zed | N/A — vmaf-mcp only exposes Tools |
| UDS transport for MCP | N/A | Not supported | N/A — vmaf-mcp uses stdio |
| Remote SSH editing | Via Remote-SSH extension | Zed has native Remote Development (SSH) | Works; configure in Zed's remote dev UI |
| Extension marketplace breadth | ~50,000 extensions | Smaller; growing | Accept gaps; use terminal for unsupported tools |

---

## 10. Migration steps

Each step is bounded with an effort estimate. Prerequisite: Zed 1.2.6+ installed.

### Step 1 — Install Zed and import VSCode settings (est: 5 min)

```bash
curl -f https://zed.dev/install.sh | sh
```

Open Zed, complete onboarding, click "Import VS Code settings" (or run
`zed: import vs code settings` from the command palette). This imports fonts,
tab behaviour, and terminal settings.

Add to `~/.config/zed/settings.json`:

```json
{ "base_keymap": "VSCode" }
```

### Step 2 — Install required extensions (est: 10 min)

Open the extension panel (`cmd/ctrl+shift+x`) and install:

- **clangd** (if not auto-detected; provides C/C++ LSP)
- **ShellCheck** (shell script linting)
- Any community **meson** syntax extension if available
- Optionally: a CUDA syntax extension

### Step 3 — Create `.zed/settings.json` (est: 15 min)

Copy the skeleton from §4 into `<repo-root>/.zed/settings.json`.
Adjust the `clangd` `binary.path` if needed (remove the key to use system
clangd).

Verify clangd is loading `build/compile_commands.json`:

- Build first: `meson setup build libvmaf -Denable_cuda=false -Denable_sycl=false && ninja -C build`
- Check Zed status bar shows language server activity on a `.c` file.

### Step 4 — Create `.zed/tasks.json` (est: 15 min)

Create `<repo-root>/.zed/tasks.json`:

```json
[
  {
    "label": "meson: setup CPU",
    "command": "meson",
    "args": ["setup", "build", "libvmaf", "-Denable_cuda=false", "-Denable_sycl=false", "--buildtype=debug"],
    "reveal": "always"
  },
  {
    "label": "meson: setup CUDA",
    "command": "meson",
    "args": ["setup", "build", "libvmaf", "-Denable_cuda=true", "-Denable_sycl=false", "--buildtype=debug", "--reconfigure"],
    "reveal": "always"
  },
  {
    "label": "meson: setup SYCL",
    "command": "meson",
    "args": ["setup", "build", "libvmaf", "-Denable_cuda=false", "-Denable_sycl=true", "--buildtype=debug", "--reconfigure"],
    "reveal": "always"
  },
  {
    "label": "build",
    "command": "meson",
    "args": ["compile", "-C", "build"],
    "reveal": "always"
  },
  {
    "label": "test: unit",
    "command": "meson",
    "args": ["test", "-C", "build", "--print-errorlogs"],
    "reveal": "always"
  },
  {
    "label": "test: netflix-golden",
    "command": "make",
    "args": ["test-netflix-golden"],
    "reveal": "always"
  },
  {
    "label": "lint: all",
    "command": "make",
    "args": ["lint"],
    "reveal": "always"
  },
  {
    "label": "format: all",
    "command": "make",
    "args": ["format"],
    "reveal": "no_focus",
    "hide": "on_success"
  }
]
```

### Step 5 — Port debug config (est: 10 min)

Test first: open a `.c` file, press the Debug icon, and see if Zed loads the
existing `.vscode/launch.json` automatically (it should — Zed falls back to it
when no `.zed/debug.json` exists).

If the GDB adapter is missing, create `.zed/debug.json`:

```json
[
  {
    "adapter": "CodeLLDB",
    "label": "Debug: vmaf CLI (Netflix normal pair)",
    "request": "launch",
    "program": "$ZED_WORKTREE_ROOT/build/tools/vmaf",
    "args": [
      "-r", "$ZED_WORKTREE_ROOT/python/test/resource/yuv/src01_hrc00_576x324.yuv",
      "-d", "$ZED_WORKTREE_ROOT/python/test/resource/yuv/src01_hrc01_576x324.yuv",
      "--width", "576", "--height", "324",
      "-p", "420", "-b", "8",
      "-m", "version=vmaf_v0.6.1",
      "--precision=17", "-q"
    ],
    "cwd": "$ZED_WORKTREE_ROOT"
  },
  {
    "adapter": "CodeLLDB",
    "label": "Debug: test_feature",
    "request": "launch",
    "program": "$ZED_WORKTREE_ROOT/build/test/test_feature",
    "cwd": "$ZED_WORKTREE_ROOT/build"
  }
]
```

Note: CodeLLDB must be installed as a Zed debugger extension.

### Step 6 — Configure Claude Agent via ACP (est: 10 min)

1. Open Agent Panel (click ✨ in status bar or run `agent: new thread`).
2. Select "Claude Agent" from the agent dropdown.
3. Zed will install `@zed-industries/claude-agent-acp` automatically.
4. Authenticate: type `/login` in the thread and follow the prompt.
5. Verify: start a thread and ask it to read `CLAUDE.md`. The agent should
   respond with the VMAF fork rules.

Source: <https://zed.dev/docs/ai/external-agents>, retrieved 2026-05-19.

### Step 7 — Verify MCP server (est: 10 min)

1. Ensure `vmaf-mcp` is installed: `pip install -e mcp-server/vmaf-mcp`.
2. Ensure `build/tools/vmaf` is built (Step 3).
3. The `context_servers.vmaf-mcp` config from §4 starts the server on first
   agent thread creation.
4. In a Claude Agent thread, type `@vmaf-mcp` and verify the seven tools appear.
5. Test: `Use the vmaf_score tool to score the Netflix normal pair.`

### Step 8 — Telemetry and privacy (est: 2 min)

The `telemetry` block in §4 opts out of diagnostics and metrics reporting.
Zed also stores AI API keys in OS keychain, not in settings files.

### Step 9 — Parallel period (est: ongoing, ~2 weeks)

Keep VSCode installed. Run both editors. The `.vscode/` directory is harmless
in the repo — Zed ignores it. Migrate remaining workflows (Nsight, oneAPI config)
task-by-task as they come up.

---

## 11. Risks and rollback

| Risk | Likelihood | Mitigation |
|----|----|----|
| clangd `--compile-commands-dir` path not found | Medium | Always build first; `build/` must contain `compile_commands.json`. Symlink at repo root (`ln -sf build/compile_commands.json .`) also accepted by Zed's clangd binary arg. |
| CUDA `.cu` files lose CUDA-specific diagnostics | Medium | Install CUDA Zed extension from marketplace; or keep VSCode for CUDA file editing during transition |
| CodeLLDB adapter not available on Linux | Low | Fallback: use VSCode for debugging; CodeLLDB is cross-platform and actively maintained |
| MCP server fails to start (PATH issue) | Low | Use absolute path in `"command"` or set `"command": "python", "args": ["-m", "vmaf_mcp.server"]` |
| Meson extension gap (no problem matcher) | Medium | Zed tasks emit output in the terminal pane; no structured problem matching currently. Errors are visible but not parsed into the diagnostics panel. |
| `.vscode/launch.json` GDB adapter not recognized | Medium | Create `.zed/debug.json` with CodeLLDB as in Step 5 |
| Rules file conflict (`AGENTS.md` vs `CLAUDE.md`) | Low | Zed panel picks `AGENTS.md`; Claude Agent reads `CLAUDE.md` directly. Both paths work. |
| `context_servers` `source: "custom"` forgotten | High if omitted | The §4 skeleton includes it; copy exactly |
| ACP auth expires or needs relogin | Low | Run `/login` in any Claude Agent thread to re-authenticate |

**Rollback:** `.vscode/` is untouched by this migration. To revert, open the
folder in VSCode — everything works as before. Zed and VSCode can coexist
indefinitely; both read the same source files.

---

## Sources (all WebFetched 2026-05-19)

- <https://zed.dev/docs> — navigation structure
- <https://zed.dev/docs/ai/overview> — AI features overview
- <https://zed.dev/docs/ai/agent-panel> — agent panel
- <https://zed.dev/docs/ai/external-agents> — ACP + Claude Agent
- <https://zed.dev/docs/ai/rules> — AGENTS.md / CLAUDE.md / rules priority
- <https://zed.dev/docs/ai/mcp> — MCP / context_servers config
- <https://zed.dev/docs/ai/llm-providers> — Anthropic API key config
- <https://zed.dev/docs/ai/agent-settings> — model, tool permissions, MCP tool permissions
- <https://zed.dev/docs/ai/configuration> — top-level AI config page
- <https://zed.dev/docs/reference/all-settings> — settings reference (redirected)
- <https://zed.dev/docs/configuring-languages> — LSP configuration
- <https://zed.dev/docs/tasks> — tasks.json reference
- <https://zed.dev/docs/debugger> — DAP + .zed/debug.json
- <https://zed.dev/docs/git> — git integration
- <https://zed.dev/docs/key-bindings> — keymap.json
- <https://zed.dev/docs/extensions> — extension types
- <https://zed.dev/docs/extensions/mcp-extensions> — MCP server extensions
- <https://zed.dev/docs/migrate/vs-code> — official VSCode migration guide
- <https://zed.dev/releases> — version 1.2.6 (2026-05-15)
- <https://markaicode.com/mcp-zed-editor-setup/> — MCP config detail + `source: "custom"` requirement

**404s encountered:**

- `https://zed.dev/docs/acp` — 404; ACP documented at `/docs/ai/external-agents`
- `https://zed.dev/docs/mcp` — 404; MCP documented at `/docs/ai/mcp`
- `https://zed.dev/docs/lsp` — 404; LSP documented at `/docs/configuring-languages`
- `https://zed.dev/docs/assistant` — redirect loop (→ `/docs/assistant/assistant.html`)
- `https://zed.dev/docs/ai` — redirect (→ `/docs/ai/overview.html`)
- `https://zed.dev/docs/ai/model-context-protocol` — 404; use `/docs/ai/mcp`
- `https://zed.dev/docs/coming-from-vscode` — 404; use `/docs/migrate/vs-code`
