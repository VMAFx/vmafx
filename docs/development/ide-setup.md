# IDE setup — VS Code and Zed (multi-language VMAFX)

This document covers the IDE configuration shipped with the VMAFX repository
for both Visual Studio Code and Zed. After the Phase 4 language modernisation
(ADR-0702 through ADR-0709) the repo contains four first-class languages — C/C++,
Go, Rust, and Python — plus Protobuf, Helm YAML, and Dockerfile surfaces. The
IDE configs handle all of them out of the box.

## Quick-start checklist

1. Install the editor of your choice (see per-editor sections below).
2. Open the repo root (`code .` or `zed .`).
3. Accept the "install recommended extensions" prompt (VS Code) or install the
   extensions listed in the Zed section below.
4. Run `meson setup build` so clangd has a `compile_commands.json` to index.
5. Run `go build ./...` once so gopls downloads its toolchain.
6. The IDE is ready.

---

## Visual Studio Code

### Required VS Code version

VS Code 1.90 or later. Earlier versions may not support the `[go]` /
`[rust]` language-scoped formatter keys.

### Recommended extensions

`.vscode/extensions.json` lists the full recommended set. VS Code surfaces a
"install recommended extensions" prompt on first open. The table below explains
the purpose of each:

| Extension ID | Purpose |
|---|---|
| `llvm-vs-code-extensions.vscode-clangd` | C/C++ language server — clangd. **Required.** MS IntelliSense is explicitly disabled. |
| `golang.go` | Official Go extension: gopls, debugger, test runner. **Required for Go.** |
| `rust-lang.rust-analyzer` | Rust language server, cargo integration, inline type hints. **Required for Rust.** |
| `zxh404.vscode-proto3` | Protobuf syntax highlighting + formatting for `proto/vmafx.proto` and `gen/`. |
| `ms-kubernetes-tools.vscode-kubernetes-tools` | Kubernetes YAML schema validation, kubectl integration for `deploy/helm/`. |
| `ms-azuretools.vscode-docker` | Dockerfile linting and container management for `docker/` and `dev/Containerfile`. |
| `mesonbuild.mesonbuild` | Meson build-file syntax and task integration. |
| `ms-python.python` | Python language support for `ai/`, `compat/python-vmaf/`, `tools/vmaf-tune/`, `scripts/`. |
| `ms-python.black-formatter` | Python formatter (black). |
| `charliermarsh.ruff` | Python linter and import sorter (ruff). |
| `nvidia.nsight-vscode-edition` | CUDA kernel profiling and debugging. |
| `intel-corporation.oneapi-environment-configurator` | oneAPI environment setup for SYCL development. |
| `foxundermoon.shell-format` | Shell script formatter (shfmt). |
| `timonwong.shellcheck` | Shell script linting (shellcheck). |
| `redhat.vscode-yaml` | YAML language server with schema validation (Helm, CI YAML). |
| `tamasfe.even-better-toml` | TOML support for `Cargo.toml`, `pyproject.toml`, etc. |
| `bierner.markdown-mermaid` | Renders Mermaid diagrams in Markdown preview — used in ADRs and architecture docs. |
| `vivaxy.vscode-conventional-commits` | Conventional Commits commit-message wizard matching the `commit-msg` hook. |
| `editorconfig.editorconfig` | Applies `.editorconfig` rules (line endings, final newline). |
| `github.vscode-github-actions` | GitHub Actions workflow linting. |
| `github.vscode-pull-request-github` | PR review UI inside VS Code. |
| `anthropic.claude-code` | Claude Code AI extension. |
| `streetsidesoftware.code-spell-checker` | Spell checking in source comments and docs. |

**Unwanted extensions** (explicitly de-recommended to suppress VS Code prompts):
`ms-vscode.cpptools`, `ms-vscode.cpptools-extension-pack` (conflicts with clangd),
`ms-vscode.cmake-tools` (we use Meson), `twxs.cmake` (dead-weight).

### C/C++ — clangd setup

`.vscode/settings.json` ships with clangd as the C/C++ language server.
Microsoft IntelliSense is explicitly disabled (`C_Cpp.intelliSenseEngine: disabled`).

clangd reads compile flags from `${workspaceFolder}/build/compile_commands.json`,
which **meson generates automatically** during `meson setup`. The
`--compile-commands-dir=${workspaceFolder}/build` argument tells clangd where to
find it.

`c_cpp_properties.json` is a minimal fallback config pointing at `core/include`
and `core/src` — clangd uses `compile_commands.json` in preference; the
`c_cpp_properties.json` fallback fires only when `build/` does not yet exist.

#### Make sure `build/` covers every backend you touch

`compile_commands.json` only contains entries for files that were compiled. If
`build/` was set up CPU-only, clangd has no include paths for `volk.h` /
`vk_mem_alloc.h` / CUDA / SYCL headers and marks every `VkInstance` /
`VmafCudaBuffer` / `sycl::queue` symbol as "undeclared identifier".

Fix: configure the IDE build with every backend you have a toolchain for:

```bash
# Vulkan-only:
meson setup build -Denable_vulkan=enabled -Denable_float=true

# CUDA + SYCL + Vulkan (full):
source /opt/intel/oneapi/setvars.sh
CC=icx CXX=icpx meson setup build \
    -Denable_cuda=true -Denable_sycl=true \
    -Denable_vulkan=enabled -Denable_float=true
```

Then **restart clangd** in VS Code (Ctrl+Shift+P → "Restart Language Server")
so it re-reads `compile_commands.json`.

#### Named per-backend build dirs alongside the IDE build

Don't reconfigure the IDE build to test a single backend. Use named dirs:

```bash
# IDE (all backends):
meson setup build  # with all -Denable_* flags for your machine

# Isolated backend test builds:
meson setup build-cuda-test -Denable_cuda=true
meson setup build-vulkan-test -Denable_vulkan=enabled
meson setup build-sycl-test -Denable_sycl=true
```

#### Symptoms of a misconfigured `build/`

- `unknown type name 'VkInstance'` on files under `core/src/feature/vulkan/`
- `unknown type name 'VmafCudaBuffer'` on files under `core/src/feature/cuda/`
- `'sycl/sycl.hpp' file not found` on files under `core/src/feature/sycl/`
- `Included header errno.h is not used directly` warnings everywhere

These all indicate that `build/` lacks entries for the relevant backend. A
CPU-only `meson setup build` is the most common root cause.

### Go — gopls

The `golang.go` extension brings gopls (Go Language Server), the go test runner,
and the Delve debugger. gopls discovers the Go workspace via `go.mod` at the repo
root — no per-directory configuration required.

Key settings in `.vscode/settings.json`:

```json
"go.lintTool": "golangci-lint",
"go.lintFlags": ["--fast"],
"go.formatTool": "gofmt",
"[go]": {
  "editor.defaultFormatter": "golang.go",
  "editor.formatOnSave": true,
  "editor.tabSize": 4,
  "editor.insertSpaces": false
}
```

Go uses tabs (not spaces). The `editor.insertSpaces: false` entry ensures VS Code
does not convert tabs to spaces on save.

**First-time setup:** after cloning, run `go build ./...` once so gopls downloads
its toolchain and populates the module cache. gopls starts automatically after this.

**Known gotcha:** gopls requires `go.mod` at the repo root (or a `go.work` file for
multi-module workspaces). The VMAFX repo uses a single `go.mod` at the root
covering `cmd/`, `pkg/`, and all Go subpackages. If you open a subdirectory
(e.g., `cmd/vmafx-server/`) as the VS Code workspace root, gopls will not find
`go.mod` and will fail to resolve imports. Always open the repo root.

### Rust — rust-analyzer

The `rust-lang.rust-analyzer` extension provides Rust LSP, inline type hints, and
`cargo clippy` integration.

Key settings:

```json
"rust-analyzer.cargo.allTargets": true,
"rust-analyzer.check.command": "clippy",
"rust-analyzer.check.extraArgs": ["-D", "warnings"],
"[rust]": {
  "editor.defaultFormatter": "rust-lang.rust-analyzer",
  "editor.formatOnSave": true
}
```

rust-analyzer discovers the workspace via `Cargo.toml` at the repo root. The root
`Cargo.toml` declares the workspace members (`bindings/rust/vmafx-sys/`,
`bindings/rust/vmafx/`).

**Known gotcha:** rust-analyzer's initial index build can take 1–2 minutes on first
open (it compiles proc-macros and builds the symbol database). Do not interpret the
"loading workspace" spinner as a hang.

**Known gotcha:** `cargo clippy -D warnings` in `check.extraArgs` means that any
`rustc` warning surfaces as an error in the VS Code Problems panel. This matches
the CI gate (`cargo clippy -D warnings`). To suppress a false positive: add a
`#[allow(...)]` annotation with a comment explaining why, rather than weakening the
settings.

### Protobuf — vscode-proto3

The `zxh404.vscode-proto3` extension provides syntax highlighting and formatting
for `proto/vmafx.proto`. File association is wired in `settings.json`:

```json
"files.associations": {
  "*.proto": "proto3"
}
```

For `buf` linting (`buf lint`), run it from the terminal — the buf VS Code
integration requires a `buf.yaml` at the workspace root (present as `proto/buf.yaml`
in this repo; `buf` picks it up when invoked from `proto/`).

### Helm + Kubernetes YAML

`ms-kubernetes-tools.vscode-kubernetes-tools` provides Helm chart linting and
`kubectl` context integration. `redhat.vscode-yaml` provides JSON schema validation
for Kubernetes manifests.

Helm templates under `deploy/helm/vmafx/templates/` use the `.tmpl` extension
which is associated to the `helm` language in `settings.json`. The Helm IntelliSense
extension provides auto-completion for `{{ .Values.* }}` and `{{ .Release.* }}`
expressions.

### Docker

`ms-azuretools.vscode-docker` provides Dockerfile linting, layer-cache
visualisation, and a container management sidebar. It auto-detects
`docker/`, `dev/Containerfile`, and `Dockerfile` at the root.

### Python

Python (`ms-python.python`) is configured to use ruff as the primary linter and
black as the formatter. The `[python]` language scope overrides:

```json
"[python]": {
  "editor.defaultFormatter": "ms-python.black-formatter",
  "editor.formatOnSave": true,
  "editor.codeActionsOnSave": {
    "source.organizeImports": "explicit"
  }
}
```

Test discovery is configured for both `python/test` and `ai/tests`:

```json
"python.testing.pytestArgs": ["python/test", "ai/tests"]
```

### Tasks (Ctrl+Shift+B / Ctrl+Shift+P → Run Task)

`.vscode/tasks.json` ships with tasks for every language layer:

| Task label | What it runs |
|---|---|
| `meson: setup CPU` | `meson setup build -Denable_cuda=false -Denable_sycl=false` |
| `meson: setup CUDA` | `meson setup build -Denable_cuda=true …` |
| `meson: setup SYCL` | `meson setup build -Denable_sycl=true …` |
| `build: C (meson)` | `meson compile -C build` (default build task) |
| `build: Go` | `go build ./...` |
| `build: Rust` | `cargo build --workspace` |
| `test: C unit` | `meson test -C build --print-errorlogs` (default test task) |
| `test: netflix-golden` | `make test-netflix-golden` |
| `test: Go` | `go test ./...` |
| `test: Rust` | `cargo test --workspace` |
| `lint: all` | `make lint` |
| `format: all` | `make format` |

### Debug configurations (`.vscode/launch.json`)

Three GDB-backed launch configurations are shipped:

- **Debug: vmafx CLI (Netflix normal pair)** — launches `build/tools/vmafx` with
  the canonical 576x324 YUV pair and `--precision=17`.
- **Debug: vmafx CLI (attach)** — GDB attach to a running process.
- **Debug: test_feature** — launches `build/test/test_feature` under GDB.

For Go debugging, the `golang.go` extension provides its own launch configuration
via the Run and Debug panel — select "Go: Launch file" or add a `launch.json`
configuration of type `"go"`.

For Rust debugging, install the `vadimcn.vscode-lldb` extension (CodeLLDB) and add
a `launch.json` entry of type `"lldb"` pointing at the compiled binary under
`bindings/rust/vmafx-sys/target/debug/`.

---

## Zed editor

Zed is supported alongside VS Code. Both `.vscode/` and `.zed/` are committed; they
coexist without interference.

### Minimum Zed version

**Zed 1.2.6 or later** (released 2026-05-15).

Install:

```bash
curl -f https://zed.dev/install.sh | sh
```

### Required Zed extensions

Open the extension panel (Ctrl+Shift+X or `zed: install extension`) and install:

| Extension | Purpose |
|---|---|
| **clangd** | C/C++ LSP (usually bundled; auto-enabled for `.c`/`.cpp`) |
| **ShellCheck** | Shell script linting via shellcheck LSP |
| **CodeLLDB** (debugger) | C/C++/Rust DAP debugger; required for `.zed/debug.json` |
| **GLSL** (community) | Syntax highlighting for `.comp`/`.vert`/`.frag` Vulkan shaders |
| CUDA (community, optional) | CUDA `.cu`/`.cuh` syntax highlighting; falls back to C++ |

Go and Rust LSPs (gopls and rust-analyzer) are **built into Zed** and activate
automatically when `go.mod` or `Cargo.toml` is detected at the workspace root — no
extension installation required.

Protobuf support is experimental in Zed. The `Proto` file-type mapping in
`.zed/settings.json` enables syntax highlighting; LSP is not yet available.

### What `.zed/settings.json` provides out-of-the-box

- **clangd LSP** with `--compile-commands-dir=build` (same flags as VS Code).
  Fallback include paths point at `./core/include` and `./core/src`.
- **gopls LSP** with `usePlaceholders: true` and `completeUnimported: true`.
- **rust-analyzer LSP** with `cargo.allTargets: true` and `clippy -D warnings`.
- **Pyright + ruff LSPs** for Python with format-on-save.
- **shfmt** as the shell-script formatter (format-on-save, 2-space, `-ci`).
- **Go language block**: `hard_tabs: true` (Go convention), format-on-save via
  language_server (gopls runs gofmt).
- **Rust language block**: 4-space, format-on-save via language_server (rust-analyzer
  runs rustfmt).
- **File-type associations**: `.cu`/`.cuh`/`.hip`/`.metal` → C++; `.proto` → Proto;
  `.comp`/`.vert`/`.frag`/`.glsl` → GLSL.
- **File scan exclusions**: `build`, `build-{cpu,san,cuda,sycl,all}`, `target`,
  `subprojects`, `.venv`, `node_modules`, `*.yuv`, `*.onnx`, `*.pkl`.
- **vmafx-mcp context server** registered under `context_servers` with
  `source: "custom"` (required by Zed).
- **Telemetry disabled** (`diagnostics: false`, `metrics: false`).

### Go in Zed — gopls LSP gotcha

gopls in Zed requires `go.mod` at the workspace root. Open the repo root, not a
subdirectory. Zed passes the worktree root to gopls as the workspace folder.

### Rust in Zed — rust-analyzer LSP gotcha

rust-analyzer activates when Zed detects `Cargo.toml` at the worktree root. The
initial workspace index build takes 1–2 minutes (Zed shows "Loading rust-analyzer"
in the status bar). The `cargo.allTargets: true` option causes rust-analyzer to
check all `[[bin]]`, `[lib]`, and `[[test]]` targets for the full picture, which
is slower but matches CI.

### Tasks in `.zed/tasks.json`

| Task label | Command |
|---|---|
| Build (CPU only) | `meson setup build … && ninja -C build` |
| Build (all backends) | `meson setup build -Denable_cuda=true -Denable_sycl=true && ninja -C build` |
| Build: Go | `go build ./...` |
| Test: Go | `go test ./...` |
| Build: Rust | `cargo build --workspace` |
| Test: Rust | `cargo test --workspace` |
| Run fast tests | `meson test -C build --suite=fast` |
| Netflix golden | `make test-netflix-golden` |
| Format all | `make format` |
| Lint all | `make lint` |
| MCP smoke test (Go) | `cd cmd/vmafx-mcp && go test ./...` |
| MCP smoke test (Python) | `cd mcp-server/vmaf-mcp && python -m pytest tests/test_smoke_e2e.py -v` |
| Open dev-mcp shell | `docker exec -it vmaf-dev-mcp bash` |

### Debug configurations (`.zed/debug.json`)

Three CodeLLDB-based debug configurations:

- **Debug: vmafx CLI (Netflix normal pair)** — launches `build/tools/vmafx` via
  CodeLLDB.
- **Debug: vmafx CLI (attach)** — CodeLLDB attach.
- **Debug: test_feature** — launches `build/test/test_feature`.

For Go debugging, Zed 1.2.6+ includes a built-in Go DAP adapter that activates
automatically for Go files. No additional configuration needed.

For Rust debugging, install the CodeLLDB Zed extension and add a debug configuration
in `.zed/debug.json` of type `"CodeLLDB"` pointing at the Rust binary under
`bindings/rust/*/target/debug/`.

### MCP server (`vmafx-mcp`)

The vmafx-mcp server uses **stdio transport** and is registered in `.zed/settings.json`
under `context_servers`:

```json
"context_servers": {
  "vmafx-mcp": {
    "source": "custom",
    "command": "vmafx-mcp",
    "env": { "VMAF_BIN": "build/tools/vmafx" }
  }
}
```

The `source: "custom"` key is **mandatory** — without it Zed silently ignores the entry.

First-time setup:

```bash
# Option A — Python server (still active during Go migration):
pip install -e mcp-server/vmaf-mcp

# Option B — Go server (recommended after ADR-0704):
go install ./cmd/vmafx-mcp
```

Then build the vmafx binary:

```bash
meson setup build -Denable_cuda=false -Denable_sycl=false && ninja -C build
```

Zed starts the server automatically on first agent thread creation. In a Claude
Agent thread, type `@vmafx-mcp` to verify the tools are listed.

### Claude Agent (ACP) in Zed

Zed auto-installs `@zed-industries/claude-agent-acp` on first use. No manual
configuration needed. Switching between Zed and VS Code does not affect Claude
Code CLI memory — per-project memory lives at `~/.claude/projects/<path>/memory/`
and is tied to the CLI process, not the editor.

### Known gaps vs VS Code (as of Zed 1.2.6, 2026-05-28)

| Feature | Status |
|---|---|
| Meson problem matcher | Not available — meson errors appear in the terminal pane only |
| Nsight profiler UI | No Zed equivalent — use `ncu` from the integrated terminal |
| oneAPI environment configurator | No Zed equivalent — source `setvars.sh` in shell profile |
| GitHub PR UI | No Zed extension — use `gh` CLI from the terminal |
| Protobuf LSP | Experimental / not yet stable in Zed |
| MCP Resources | Not supported in Zed; vmafx-mcp only exposes Tools so no impact |

---

## Cross-editor notes

VS Code and Zed coexist indefinitely. Both `.vscode/` and `.zed/` are committed to
the repository and maintained in sync. Switching between the two editors is safe at
any point — they read the same source files, the same `go.mod`, the same
`Cargo.toml`, and the same `build/compile_commands.json`.

The `vmafx-dev-mcp` Docker container (see
[docs/development/dev-mcp.md](dev-mcp.md)) is the recommended execution environment
for non-trivial vmafx / vmafx-tune / AI / MCP runs. The IDE configs complement the
container rather than replacing it: the IDE indexes the source on the host, while
heavy compilation and test runs go through the container.
