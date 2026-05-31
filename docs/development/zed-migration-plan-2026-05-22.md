<!-- markdownlint-disable MD013 MD060 -->
# Zed Migration Plan — VMAFx/vmafx fork (2026-05-22 refresh)

Refresh of [`zed-migration-plan-2026-05-19.md`](zed-migration-plan-2026-05-19.md)
after the Zed 1.3.5 (2026-05-20) and 1.3.6 (2026-05-21) releases.
Backing audit: [`docs/research/0729-zed-config-1-3-6-refresh.md`](../research/0729-zed-config-1-3-6-refresh.md).

The 2026-05-19 plan remains in tree as a dated snapshot. **This doc is the
current reference.**

Every Zed-feature claim cites a WebFetched URL with retrieval date 2026-05-22.

---

## 0. What changed since 2026-05-19

### 0.1 Zed version

- **2026-05-19 plan:** Zed 1.2.6 latest.
- **2026-05-22 actual:** Zed **1.3.6** latest (released 2026-05-21).

Releases between the two plans that move project-relevant surface:

| Version | Date | Key change relevant here |
|---|---|---|
| 1.3.5 | 2026-05-20 | **`subagent_model` setting (new)**; Terminal Threads (start agent from terminal pane); agent renders inline images + Mermaid; Git panel branch history; custom Git command support |
| 1.3.6 | 2026-05-21 | Google thinking-level support; Gemini 3.5 Flash registered; npm-backed tool installs respect release-age filters |

Source: <https://zed.dev/releases>, retrieved 2026-05-22.

### 0.2 Settings keys new vs the 2026-05-19 plan

The 2026-05-19 plan referenced `default_model` and `tool_permissions`. The
live agent-settings reference now documents **seven additional `agent.*`
keys** (full set: <https://zed.dev/docs/ai/agent-settings>, retrieved
2026-05-22). The repo config in this commit wires them all.

| Key | Purpose | This repo's pin |
|---|---|---|
| `default_model` | Headline agent model | Anthropic Sonnet 4.5 |
| `inline_assistant_model` | `Ctrl+Enter` inline rewrites | Anthropic Haiku 4.5 |
| `commit_message_model` | Git commit message gen | Anthropic Haiku 4.5 |
| `thread_summary_model` | Thread title / summary | Anthropic Haiku 4.5 |
| `subagent_model` *(new in 1.3.5)* | Spawned subagent default | Anthropic Sonnet 4.5 (explicit, not inherit) |
| `inline_alternatives` | Parallel competing-model outputs | (not pinned — per-user) |
| `model_parameters` | Per-model temperature / top_p | (not pinned — per-user) |

The rationale for the split is documented in
[`docs/research/0729-zed-config-1-3-6-refresh.md`](../research/0729-zed-config-1-3-6-refresh.md):
inline assistant fires on every `Ctrl+Enter` selection rewrite; routing it to
Sonnet 4.5 burns subscription quota fast on what is usually a small edit.

### 0.3 `tool_permissions` schema change

Live docs (<https://zed.dev/docs/ai/agent-settings>, retrieved 2026-05-22) document a
**regex-based** per-tool form using `always_allow` / `always_deny` /
`always_confirm` keys plus a `case_sensitive` toggle.

The 2026-05-19 plan and the previous `.zed/settings.json` used a `default: allow`
per-tool form, which Zed tolerates but is not the documented surface. This
commit converts to regex form.

### 0.4 `agent_servers` ACP registry pin

The 2026-05-19 plan documented `context_servers` (MCP) but not
`agent_servers` (ACP). Live docs (<https://zed.dev/docs/ai/external-agents>,
retrieved 2026-05-22) describe the **ACP Registry** (v0.221.x+) as the
preferred distribution method for external CLI agents.

This commit adds an `agent_servers` block to `.zed/settings.json` with three
registry agents so a fresh clone gets the same Claude / Codex / Gemini ACP
adapters regardless of the teammate's global config:

```jsonc
"agent_servers": {
  "claude-acp": { "type": "registry", "default_model": "claude-sonnet-4-5" },
  "codex-acp":  { "type": "registry" },
  "gemini":     { "type": "registry" }
}
```

Auth is per-agent and decoupled from Zed settings:

- **Claude Agent** — `/login` inside the thread (Anthropic API key or
  Claude Pro/Max subscription).
- **Codex CLI** — `/login` for ChatGPT, or `CODEX_API_KEY` / `OPENAI_API_KEY`
  env var; also reads `~/.codex/config.toml`.
- **Gemini CLI** — interactive Google sign-in or `GEMINI_API_KEY` env var.

Source: <https://zed.dev/docs/ai/external-agents>, retrieved 2026-05-22.

### 0.5 Edit prediction `disabled_globs`

Live docs (<https://zed.dev/docs/ai/edit-prediction>, retrieved 2026-05-22)
document a `disabled_globs` filter that suppresses prediction requests on
matching paths.

This commit adds a blocklist for vendor / golden / corpus paths so the
prediction provider (Zeta / Copilot / Codestral, per teammate's choice)
never receives the contents of:

- `build/**`, `build-*/**`, `subprojects/**`
- `python/test/resource/**`, `python/vmaf/resource/**`, `python/vmaf/matlab/**`
- `.workingdir/**`, `.workingdir2/**`, `.corpus/**`, `model/**`
- `**/*.yuv`, `**/*.onnx`, `**/*.onnx.data`, `**/*.pkl`, `**/*.parquet`,
  `**/*.bin`

### 0.6 Tasks and debug — coverage gaps closed

`.zed/tasks.json` now includes 12 additional entries beyond the 15 the
2026-05-19 plan landed:

- Container-first workflows (rebuild dev-mcp; build / fast-test inside the
  container) — aligns with [`CLAUDE.md §12 r15`](../../CLAUDE.md)'s default.
- ADR helpers (`adr: claim next number` using `$ZED_SELECTED_TEXT`).
- Cross-backend correctness (`validate scores: all backends, Netflix normal pair`).
- Python sub-tree quick gates (pytest current file, ruff current file).
- Doc gates (`mkdocs: build strict`, `regen docs`).
- Local PR gate (`deliverables-check vs current PR body`).
- `format-check` (non-mutating CI-equivalent).

`.zed/debug.json` gains five Debugpy / CodeLLDB entries:

- Debugpy on pytest current file
- Debugpy on `ai/tests`
- Debugpy on vmaf-mcp server (stdio)
- Debugpy on vmaf-tune compare
- CodeLLDB on `build-san/test/test_feature` with ASan + UBSan env

### 0.7 File-type association — `.hip`, `.metal`, `.mm`, Meson

`.zed/settings.json` `file_types` now maps:

- `*.cu`, `*.cuh`, `*.hip`, `*.metal`, `*.mm` → `C++` (clangd handles all five
  as C++/Objective-C++ via `compile_commands.json`).
- `*.comp`, `*.vert`, `*.frag`, `*.glsl` → `GLSL`.
- `meson.build`, `meson_options.txt` → `Python` (stopgap — no Zed Meson
  extension is verified yet; Python's syntax is closer to Meson than Bash's).

Track upstream Meson extension at <https://github.com/zed-industries/extensions>;
no resolution as of 2026-05-22.

### 0.8 External formatters resolve via project `.venv/bin/`

Zed runs `format_on_save` external formatters as subprocesses with the
workspace root as CWD. The 2026-05-19 settings invoked them as bare
`"command": "ruff"` / `"command": "shfmt"`, which required the host's
`$PATH` to include them. On a fresh host the system PATH typically has
neither.

Both binaries are already provisioned in the project's `.venv/bin/`
(ruff 0.15.10+, shfmt 3.13.1+, black 26.3.1+, mypy 1.20.1+ — see the
project venv readme for refresh steps). The 2026-05-22 refresh switches
the external-formatter commands to that relative path:

```jsonc
"Python":  { "formatter": [{ "external": { "command": ".venv/bin/ruff",  ... }}] }
"Shell Script": { "formatter":  { "external": { "command": ".venv/bin/shfmt", ... }}}
```

If your clone has no `.venv` yet, create it with:

```bash
python3 -m venv .venv
.venv/bin/pip install ruff black mypy
# shfmt is a Go binary; install via system package or:
go install mvdan.cc/sh/v3/cmd/shfmt@latest
cp $(go env GOPATH)/bin/shfmt .venv/bin/
```

Zed's LSP-side `ruff` integration (under the `lsp.ruff` block) uses the
Zed-extension-managed ruff binary, which auto-downloads independent of
the venv. The venv copy is only used for the post-save formatter
external command.

### 0.9 `context_servers.vmaf-mcp` — container-exec wiring

The 2026-05-19 plan landed a host-side MCP invocation
(`"command": "vmaf-mcp"`) that assumed the binary was on `$PATH` after
`pip install -e mcp-server/vmaf-mcp/`. This refresh switches to a
container-exec form against the `vmaf-dev-mcp` service defined in
[`dev/docker-compose.yml`](../../dev/docker-compose.yml):

```jsonc
"context_servers": {
  "vmaf-mcp": {
    "source": "custom",
    "command": "docker",
    "args": ["exec", "-i", "vmaf-dev-mcp", "vmaf-mcp"]
  }
}
```

Rationale: aligns with [`CLAUDE.md §12 r15`](../../CLAUDE.md) ("default to
the `vmaf-dev-mcp` container") and removes the host-side install
prerequisite. The container's `vmaf-mcp` resolves `VMAF_BIN` to
`/usr/local/bin/vmaf` (its candidate #1), so no env override is needed.

Prerequisite: the dev container must be up before Zed spawns the MCP.
The compose service is `restart: unless-stopped`, so:

```bash
docker compose -f dev/docker-compose.yml up -d dev-mcp
```

is a once-per-host action. If the container is down when Zed launches,
the MCP entry shows an error in the Agent Panel — fix is to start the
container and reload the workspace (`workspace: reload`).

Smoke test (host shell, container running):

```bash
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"smoke","version":"0"}}}' \
  | docker exec -i vmaf-dev-mcp vmaf-mcp
# → {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05", ...}}
```

### 0.10 Extension manifest — paste into user-global settings

`auto_install_extensions` in Zed is **user-global-only** — verified
against [`crates/extension_host/src/extension_host.rs`](https://github.com/zed-industries/zed/blob/main/crates/extension_host/src/extension_host.rs)
which reads via `ExtensionSettings::get_global(cx)` and triggers the
install pass exactly once from `ExtensionStore::new()`. The setting is
silently ignored when placed in project-scope `.zed/settings.json`.

So this is a manifest, not a config block. Paste the following into
`~/.config/zed/settings.json` (top level) and **fully quit + reopen
Zed** to install:

```jsonc
"auto_install_extensions": {
  "glsl":         true,   // Vulkan compute kernels (.comp/.vert/.frag — 71 files)
  "shader-ls":    true,   // Shader Language Server (HLSL + GLSL diagnostics)
  "neocmake":     true,   // CMake grammar + LSP (.cmake — vendored subprojects)
  "editorconfig": true,   // .editorconfig at repo root
  "ruff":         true,   // Official Zed Ruff LSP integration
  "dockerfile":   true,   // dev/Containerfile + Dockerfile.ffmpeg
  "meson":        true,   // meson.build / meson_options.txt (primary build system)
  "toml":         true,   // pyproject.toml + Cargo.toml-style configs
  "make":         true,   // Makefile (CI parity targets)
  "log":          true    // build / sanitizer / vmaf-dev-mcp .log files
}
```

CUDA / HIP / Metal source files (`*.cu`, `*.cuh`, `*.hip`, `*.metal`,
`*.mm`) keep the existing `file_types: { C++: [...] }` mapping —
clangd handles them as C++/Objective-C++ via `compile_commands.json`,
and there is no published Zed extension for CUDA / HIP / Metal grammar
as of 2026-05-22. The GLSL extension covers our Vulkan compute kernels.

Debug adapters (`debugpy`, `CodeLLDB`) referenced in `.zed/debug.json`
are NOT extensions — they are first-party DAP integrations bundled in
Zed core and installed lazily on the first debug session. No
`auto_install_extensions` entry is needed for them.

Snippets remain global-only (see §4). A future fork-level snippet pack
(ADR scaffold, conventional commit prefix, model card outline) is
tracked as a deferred follow-up.

---

## 1. Quick start (post-refresh)

For a teammate setting up a fresh clone for Zed:

```bash
# 1. Install Zed (1.3.6+ required for subagent_model)
curl -f https://zed.dev/install.sh | sh

# 2. Open the repo
cd /path/to/vmaf
zed .

# 3. (Optional) import VSCode settings if you have a VSCode profile
#    Command Palette → "zed: import vs code settings"

# 4. Set base keymap globally if you want VSCode-style shortcuts
echo '{ "base_keymap": "VSCode" }' >> ~/.config/zed/settings.json
#    (or "Cursor", "JetBrains", "Atom", "TextMate", "SublimeText", "Emacs")

# 5. Authenticate the external agents you'll use. Open the Agent Panel,
#    select an agent (claude-acp / codex-acp / gemini), run /login.

# 6. Build at least once so clangd has compile_commands.json:
meson setup build -Denable_cuda=false -Denable_sycl=false
ninja -C build

# 7. (Optional) Build inside the container instead:
#    Run the "container: rebuild dev-mcp image + up" task,
#    then "container: build vmaf (all backends)".
```

---

## 2. Multi-agent workflow (Claude + Codex + Gemini)

The user maintains subscriptions for Claude, Codex, and Gemini. The ACP
registry block in `.zed/settings.json` makes all three available in the
Agent Panel without per-teammate setup.

Practical patterns:

- **Claude Agent** — primary agent for this repo. Reads `CLAUDE.md` +
  per-directory `AGENTS.md` natively. Runs `.claude/skills/` (e.g.
  `/build-vmaf`, `/add-gpu-backend`) and `.claude/hooks/` because they
  live in the Claude Code CLI process.
- **Codex CLI** — useful as a parallel worker when Claude limits hit.
  Reads `~/.codex/config.toml`. Has access to the same `vmaf-mcp`
  context server via ACP forwarding.
- **Gemini CLI** — third parallel lane. Good for one-shot Q&A or doc-style
  work. Also gets `vmaf-mcp` via ACP.

When splitting work across agents:

- Each agent runs in **its own thread** in the panel; threads have
  isolated context windows.
- Per the `feedback_agents_isolated_worktree_only` memory rule for this
  repo, background agents should run in isolated git worktrees
  (`isolation: "worktree"` when spawned). This applies whether the agent
  is Claude, Codex, or Gemini.
- Tool permissions in `.zed/settings.json` apply only to Zed's first-party
  agent. External agents (Claude / Codex / Gemini ACP) request permission
  at runtime through their own UI.

---

## 3. Rules file priority (unchanged from 2026-05-19 plan)

Live docs (<https://zed.dev/docs/ai/rules>, retrieved 2026-05-22) confirm
Zed reads the **first matching file at the project root** from:

```text
1. .rules
2. .cursorrules
3. .windsurfrules
4. .clinerules
5. .github/copilot-instructions.md
6. AGENT.md
7. AGENTS.md          ← this repo, used by Zed first-party panel
8. CLAUDE.md          ← this repo, used by Claude Agent ACP directly
9. GEMINI.md
```

Only the first match is used. This repo has both `AGENTS.md` (Zed panel
picks this) and `CLAUDE.md` (Claude Agent ACP reads independently).
**Subdirectory `AGENTS.md` files (28 of them under
`libvmaf/`, `ai/`, `tools/`, …) are NOT read by Zed's panel.** They are
read by Claude Code CLI when it walks the directory tree during a session.

This is intentional and correct; no action needed.

---

## 4. Snippets — global-only path (workaround)

Live docs (<https://zed.dev/docs/snippets>, retrieved 2026-05-22) confirm
snippets live at `~/.config/zed/snippets/` (global). **No project-local
snippet path is documented.**

If we want team-shared project snippets (ADR scaffold, conventional commit
subject, model card outline, feature extractor template), the workaround
is to ship `.zed/snippets/*.json` in-tree and have teammates symlink:

```bash
ln -s "$(pwd)/.zed/snippets/python.json" ~/.config/zed/snippets/vmaf-python.json
ln -s "$(pwd)/.zed/snippets/markdown.json" ~/.config/zed/snippets/vmaf-markdown.json
```

This is a workaround, not a fix. Tracked as deferred follow-up.

---

## 5. Codex / Gemini quota note

The user's Codex subscription is on quota limit through Tuesday
(2026-05-26). The ACP registry pin still works — the adapter is installed
regardless. Practically: when running parallel agents, route Codex-bound
threads to a different agent until limits reset, or wait until 2026-05-26.

Gemini limits are independent. Free-tier limits per Google account; with
the user's subscription, the practical bottleneck is rate-limiting, not
quota.

---

## 6. Risks and rollback

Same as the 2026-05-19 plan. Adding:

| Risk | Likelihood | Mitigation |
|---|---|---|
| `subagent_model` is ignored on Zed < 1.3.5 | Low (anyone on the team) | Bump local Zed to 1.3.6 before using subagents; Zed warns about unknown keys but does not refuse to start. |
| ACP adapter version drift (npm-backed) | Low | 1.3.6 added release-age filter respect; pin via `agent_servers.<name>.default_mode` if needed. |
| `disabled_globs` accidentally hides too much | Medium | Reviewed list; matches `.gitignore` family for vendor / golden / corpus content. |
| Regex `tool_permissions` typo locks out a tool | Medium | Test each rule by invoking the tool in an agent thread; revert via JSON edit. |
| Codex / Gemini auth fails first time | Low | Each is a `/login` in the thread; not Zed-wide blocker. |

**Rollback for this PR specifically:** revert the four files
(`.zed/{settings,tasks,debug}.json` + `docs/development/zed-migration-plan-2026-05-22.md`).
The 2026-05-19 plan stays as the previous reference. No code is touched.

---

## 7. References (live, retrieved 2026-05-22)

- <https://zed.dev/docs/ai/overview>
- <https://zed.dev/docs/ai/external-agents>
- <https://zed.dev/docs/ai/mcp>
- <https://zed.dev/docs/ai/rules>
- <https://zed.dev/docs/ai/agent-panel>
- <https://zed.dev/docs/ai/edit-prediction>
- <https://zed.dev/docs/ai/agent-settings>
- <https://zed.dev/docs/ai/inline-assistant>
- <https://zed.dev/docs/ai/llm-providers>
- <https://zed.dev/docs/configuring-languages>
- <https://zed.dev/docs/tasks>
- <https://zed.dev/docs/snippets>
- <https://zed.dev/docs/debugger>
- <https://zed.dev/docs/migrate/vs-code>
- <https://zed.dev/releases>

Audit + decision matrix:
[`docs/research/0729-zed-config-1-3-6-refresh.md`](../research/0729-zed-config-1-3-6-refresh.md)

Prior plan (snapshot, do not modify):
[`docs/development/zed-migration-plan-2026-05-19.md`](zed-migration-plan-2026-05-19.md)
