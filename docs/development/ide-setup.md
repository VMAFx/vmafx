<!-- markdownlint-disable MD025 MD060 -->
# IDE setup (VS Code + clangd)

# IDE setup (VS Code + Zed + clangd)

`.vscode/settings.json` ships with clangd as the C/C++ language
server (Microsoft IntelliSense is explicitly disabled). clangd
reads compile flags from `${workspaceFolder}/build/compile_commands.json`
which **meson generates automatically** during `meson setup`.

## Make sure `build/` covers every backend you touch

`compile_commands.json` only contains entries for files that
were actually compiled. If `build/` was set up CPU-only, clangd
has no include paths for CUDA / SYCL headers and lights up every
`VmafCudaBuffer` / `sycl::queue` symbol as "undeclared identifier".

> **Vulkan removed (ADR-0726):** The `enable_vulkan` option no longer exists.
> Do not pass `-Denable_vulkan=enabled`; meson will reject it as an unknown
> option. The `volk.h` / `vk_mem_alloc.h` / `VkInstance` / `VkDevice`
> warnings in old `build/` directories are resolved by reconfiguring without
> that flag.

Fix: configure the IDE build with every backend you have a
toolchain for:

```bash
# CUDA + SYCL (full GPU IDE build):
source /opt/intel/oneapi/setvars.sh
CC=icx CXX=icpx meson setup build \
    -Denable_cuda=true -Denable_sycl=true \
    -Denable_float=true
```

Then **restart clangd** in VS Code (Ctrl+Shift+P → "Restart
Language Server") so it re-reads `compile_commands.json`.

## If you need separate build dirs for backend-specific testing

Don't lose the IDE build to an enable-only-one-backend
reconfigure. Use named per-backend dirs alongside the IDE one:

```bash
# IDE (everything enabled):
meson setup build  -D…all-backends-on…

# CUDA-only test build:
meson setup build-cuda-test -Denable_cuda=true -Denable_float=true

# SYCL-only test build (icx/icpx required):
meson setup build-sycl-test -Denable_sycl=true -Denable_float=true
```

The cross-backend gate scripts under `scripts/ci/` accept any
of these via `--vmaf-binary`.

## Symptoms of a misconfigured `build/`

- `unknown type name 'VmafCudaBuffer'` / `VmafCudaState'` on
  files under `core/src/feature/cuda/`.
- `'sycl/sycl.hpp' file not found` on files under
  `core/src/feature/sycl/`.
- `Included header errno.h is not used directly` warnings (a
  consequence of the indexer giving up after the first error).

If you see these and clangd is otherwise working, the cause is
99% of the time that `build/` was set up without the relevant
backend.

---

## Zed editor

Zed is supported alongside VS Code. Both editors coexist — `.vscode/` and
`.zed/` are both committed and neither interferes with the other.

### Minimum Zed version

**Zed 1.2.6 or later** (released 2026-05-15).
Source: <https://zed.dev/releases>, retrieved 2026-05-19.

Install:

```bash
curl -f https://zed.dev/install.sh | sh
```

### Project config that ships with this repo

`.zed/settings.json` provides out-of-the-box:

- **clangd LSP** pointing at `build/compile_commands.json` with the same
  flags as `.vscode/settings.json` (`--compile-commands-dir=build`,
  `--background-index`, `--clang-tidy`, etc.).
- **Pyright + ruff LSPs** for Python with format-on-save via ruff.
- **shfmt** as the shell-script formatter (format-on-save).
- **MCP server** (`vmaf-mcp`) registered under `context_servers` with
  `source: "custom"` (required by Zed — see §MCP below).
- **Claude Agent** via ACP — Zed auto-installs `@zed-industries/claude-agent-acp`
  on first use; no manual configuration needed.
  Source: <https://zed.dev/docs/ai/external-agents>, retrieved 2026-05-19.
- **File-type associations**: `.cu`/`.cuh`/`.hip`/`.metal` → C++;
  `.comp`/`.vert`/`.frag`/`.glsl` → GLSL.
- **Telemetry disabled** (`diagnostics: false`, `metrics: false`).

`.zed/tasks.json` mirrors all `Makefile` targets and key `meson` invocations:

| Task label | Command |
|---|---|
| Build (CPU only) | `meson setup build … && ninja -C build` |
| Build (all backends) | `meson setup build -Denable_cuda=true -Denable_sycl=true && ninja -C build` |
| Run fast tests | `meson test -C build --suite=fast` |
| Netflix golden | `make test-netflix-golden` |
| Format all | `make format` |
| Lint all | `make lint` |
| Open dev-mcp shell | `docker exec -it vmaf-dev-mcp bash` |
| MCP smoke test | `cd mcp-server/vmaf-mcp && python -m pytest tests/test_smoke_e2e.py -v` |

`.zed/debug.json` provides CodeLLDB-based debug configurations for the vmaf
CLI and C unit tests. Zed also auto-loads `.vscode/launch.json` as a fallback
when `.zed/debug.json` is present; `.zed/debug.json` takes precedence.
Source: <https://zed.dev/docs/debugger>, retrieved 2026-05-19.

### Required Zed extensions

Open the extension panel (Ctrl+Shift+X) and install:

| Extension | Purpose |
|---|---|
| **clangd** | C/C++ language server (may be auto-detected) |
| **ShellCheck** | Shell script linting via shellcheck LSP |
| **CodeLLDB** (debugger) | C/C++ DAP debugger adapter; required for `.zed/debug.json` |
| **GLSL** (community) | Syntax highlighting for `.comp`/`.vert`/`.frag` Vulkan shaders |
| CUDA (community, optional) | CUDA-specific syntax highlighting for `.cu`/`.cuh`; otherwise C++ highlighting is used as a stopgap |
| Meson (community, optional) | Meson build file syntax; not yet in the marketplace as of 2026-05-19 — map `.build`/`meson.build` files manually if available |

Extension IDs verified against the Zed marketplace 2026-05-19:

- `zed-industries/clangd` (bundled with Zed, auto-enabled for C/C++)
- `zed-industries/shellcheck` (from Zed extensions registry)
- CodeLLDB: install from Zed debugger extension panel

### Recommended global settings (`~/.config/zed/settings.json`)

```json
{
  "base_keymap": "VSCode",
  "vim_mode": false
}
```

Source: <https://zed.dev/docs/migrate/vs-code>, retrieved 2026-05-19.

### Claude Agent (ACP) and memory continuity

Switching from VS Code to Zed does **not** affect Claude Code CLI memory or
context. Claude Code stores its per-project memory at
`~/.claude/projects/<path>/memory/` (and cross-session auto-memory at the same
location). This memory persists across editors because it is tied to the Claude
Code CLI process, not to any editor extension. When Claude Agent runs inside
Zed via ACP, it is the same `claude` CLI process reading the same memory files.

Skills (`.claude/skills/`), hooks (`.claude/hooks/`), and all ADR/plan state
under `.workingdir2/` are equally unaffected — they are filesystem artifacts
read by the CLI, not by the editor.
Source: <https://zed.dev/docs/ai/external-agents>, retrieved 2026-05-19 (ACP
architecture: Zed forwards project root and env to the CLI process).

### MCP server (`vmaf-mcp`)

The vmaf-mcp server uses **stdio transport** and works natively with Zed's
`context_servers` key (no Unix domain socket bridge needed).
Source: <https://zed.dev/docs/ai/mcp>, retrieved 2026-05-19.

First-time setup:

```bash
pip install -e mcp-server/vmaf-mcp
```

Then build the vmaf binary so `VMAF_BIN=build/tools/vmaf` resolves:

```bash
meson setup build -Denable_cuda=false -Denable_sycl=false && ninja -C build
```

Zed starts the server automatically on first agent thread creation. In a
Claude Agent thread, type `@vmaf-mcp` to verify the tools are listed.

**Important**: the `source: "custom"` key in `context_servers` is **mandatory**.
Without it, Zed silently ignores the entry.
Source: <https://markaicode.com/mcp-zed-editor-setup/>, retrieved 2026-05-19.

Zed supports MCP **Tools and Prompts** only; MCP Resources are not yet
implemented in Zed. This is not a blocker since vmaf-mcp only exposes Tools.

### Known gaps vs VS Code (as of Zed 1.2.6, 2026-05-19)

| Feature | Status |
|---|---|
| Meson problem matcher | Not available — meson errors appear in the terminal pane but are not parsed into the diagnostics panel |
| Nsight profiler UI (`nvidia.nsight-vscode-edition`) | No Zed equivalent — use `ncu` from the integrated terminal |
| oneAPI environment configurator | No Zed equivalent — source `setvars.sh` in shell profile; tasks inherit the env |
| GitHub PR UI (`github.vscode-pull-request-github`) | No Zed extension — use `gh` CLI from the terminal |
| `launch.json` GDB adapter | Zed's DAP prefers CodeLLDB; `.zed/debug.json` provides CodeLLDB configs |
| MCP Resources | Not supported in Zed; vmaf-mcp does not expose Resources so no impact |

VS Code and Zed coexist indefinitely — `.vscode/` is unchanged and fully
functional. The parallel-period strategy from the migration plan (see
`docs/development/zed-migration-plan-2026-05-19.md`) recommends running both
editors for approximately two weeks to validate workflows before switching
fully.
