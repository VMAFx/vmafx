<!-- markdownlint-disable MD013 MD060 -->
# IDE setup (VS Code + Zed + clangd)

`.vscode/settings.json` ships with clangd as the C/C++ language
server (Microsoft IntelliSense is explicitly disabled). clangd
reads compile flags from `${workspaceFolder}/build/compile_commands.json`,
which Meson generates during `meson setup`.

## Make sure `build/` covers every backend you touch

`compile_commands.json` only contains entries for files that
were actually compiled. If `build/` was set up CPU-only, clangd
has no include paths for CUDA / SYCL headers and lights up every
`VmafCudaBuffer` / `sycl::queue` symbol as "undeclared identifier".

> **Vulkan removed (ADR-0726):** The `enable_vulkan` option no longer exists.
> Do not pass `-Denable_vulkan=enabled`; Meson will reject it as an unknown
> option. Warnings about `volk.h`, `vk_mem_alloc.h`, `VkInstance`, or
> `VkDevice` mean that the build directory predates the removal and should be
> reconfigured.

Configure the IDE build with every backend for which the host has a toolchain:

```bash
# CUDA + SYCL (full GPU IDE build)
source /opt/intel/oneapi/setvars.sh
CC=icx CXX=icpx meson setup build core \
    -Denable_cuda=true -Denable_sycl=true \
    -Denable_float=true
```

Then restart clangd so it re-reads `compile_commands.json`.

## If you need separate backend build directories

Do not turn the IDE build into an enable-only-one-backend directory. Use named
build directories alongside it:

```bash
# CUDA-only test build
meson setup build-cuda-test core \
    -Denable_cuda=true -Denable_sycl=false -Denable_float=true

# SYCL-only test build (icx/icpx required)
CC=icx CXX=icpx meson setup build-sycl-test core \
    -Denable_cuda=false -Denable_sycl=true -Denable_float=true
```

The cross-backend gate scripts under `scripts/ci/` accept a selected binary
through `--vmaf-binary`.

## Symptoms of a misconfigured `build/`

- `unknown type name 'VmafCudaBuffer'` / `VmafCudaState` under
  `core/src/feature/cuda/`;
- `'sycl/sycl.hpp' file not found` under `core/src/feature/sycl/`; or
- a cascade of include-cleanliness warnings after clangd's first fatal error.

If clangd otherwise works, regenerate the compilation database with the
backend enabled before changing source includes.

## Zed editor

The checked-in contract targets **Zed 1.18.1**
(`bebe92f469834a287f5a57ed78e8d51a918b8ada`). It was verified against that
exact upstream source revision and the current official Zed documentation on
2026-09-24. `.zed/` and `.vscode/` coexist; neither editor needs the other's
configuration.

### Project settings versus user settings

Zed parses `.zed/settings.json` with its restricted project-settings schema.
The repository therefore keeps only settings that can actually be applied to
this worktree:

- clangd reads `build/compile_commands.json` and uses the repository's own
  `.clang-tidy` policy;
- CUDA, HIP, Metal, and Objective-C++ suffixes map to C++, while Meson files
  map to the Meson language;
- edit predictions are disabled for models, corpora, golden fixtures,
  `.workingdir/`, build output, and binary dataset formats;
- file scanning preserves Zed's inherited defaults through the `"..."`
  entry and additionally skips large build, model, and corpus artefacts; and
- the `vmafx-mcp` context server starts the current Go MCP binary inside the
  dev container.

Project settings cannot install ACP agents or set native-agent permissions.
`agent`, `agent_servers`, UI preferences, telemetry, and extension installation
belong in the developer's user settings. Keeping them out of the repository is
also a trust boundary: cloning a project must not silently choose a provider,
model, or permission policy for a developer.

Zed opens new worktrees in Restricted Mode. Trust the worktree only after
reviewing `.zed/settings.json`; until then Zed deliberately will not launch the
project's language or MCP servers.

### Extensions

Install the Meson extension for the `Meson` file association. C, C++, Go,
Python, CodeLLDB, Debugpy, and Delve support are built into Zed or installed on
first use. The repository does not use `auto_install_extensions`: Zed reads
that key from user settings only.

### External agents (ACP)

Open `zed: acp registry` and install whichever external agents you use. The
current registry IDs for the team's three common clients are `claude-acp`,
`codex-acp`, and `gemini`. If a developer prefers a declarative user-level
manifest, this is the supported shape in `~/.config/zed/settings.json`:

```json
{
  "agent_servers": {
    "claude-acp": { "type": "registry" },
    "codex-acp": { "type": "registry" },
    "gemini": { "type": "registry" }
  }
}
```

Do not add provider model IDs here. External ACP agents own their authentication,
subscription, model selection, and native permission policy. Zed's
`agent.tool_permissions` applies to the native Zed Agent and is user-global;
it is not a project-level substitute for an external agent's write mode.

### Dev container and MCP

For vmaf, vmaf-tune, AI, and MCP work, build the exact-source image first and
then start the service from Zed's task picker:

1. `Dev container: build exact source`
2. `Dev container: start`
3. `MCP: probe running container`

The context-server process is equivalent to:

```bash
docker exec -i vmaf-dev-mcp vmafx-mcp
```

The server is the Go binary from `cmd/vmafx-mcp` (ADR-1229). The old Python
`vmaf-mcp` command is retained only as a compatibility surface and is not the
IDE default. Current Zed custom MCP entries use `command` plus `args`; the old
`source: "custom"` field is not part of the 1.18.1 shape.

### Checked-in tasks

`.zed/tasks.json` keeps the three governance tasks introduced by the HISS
adoption and adds current, executable workflows:

| Task | Purpose |
|---|---|
| `Standards: Verify All` | Run the aggregate local governance gate. |
| `Standards: Audit` | Run the standards audit. |
| `Standards: Compile Context` | Verify generated standards context. |
| `Dev container: build exact source` | Build with the source-revision guard. |
| `Dev container: start` | Start only the primary `dev-mcp` service. |
| `Dev container: CPU fast gate` | Build in writable `/probes/zed-build-cpu` and run the fast suite. |
| `MCP: probe running container` | Exercise the running container and write its normal probe receipt. |
| `vmaf-tune: compare CPU smoke` | Run the current `vmaf-tune compare --src ...` CLI in the container. |
| `Netflix golden`, `Format check`, `Lint all` | Invoke canonical Make targets. |
| `pytest: current file` | Run the active Python test through the selected environment. |
| `ADR: claim selected slug` | Reserve an ADR number through the atomic allocator. |

The container mounts `/workspace` read-only, so the fast-gate task writes its
build directory under `/probes`; a task that tries to create `/workspace/build`
will fail by design.

### Checked-in debug scenarios

`.zed/debug.json` provides:

- CodeLLDB launch and attach scenarios for the `vmaf` CLI;
- CodeLLDB scenarios for `test_feature` in normal and ASan/UBSan builds;
- Debugpy for the active pytest file and the current `vmaftune.cli` module; and
- Delve for the Go `cmd/vmafx-mcp` server.

Zed only falls back to `.vscode/launch.json` when no `.zed/debug.json`
configurations exist, so the checked-in Zed list must remain complete enough
for the advertised workflows.

### Regression check

Run the focused contract after editing any `.zed/` file or this guide:

```bash
python3 -m pytest -q scripts/ci/tests/test_zed_project_config.py
```

The test rejects project-ignored `agent` / `agent_servers` keys, the retired
numbered workspace root, the deprecated Python MCP entrypoint, deleted helper
paths, the pre-package `vmaf-tune` module path, and loss of the governance
tasks.

### Authoritative Zed references

- [Project settings parser at the verified 1.18.1 commit](https://github.com/zed-industries/zed/blob/bebe92f469834a287f5a57ed78e8d51a918b8ada/crates/settings_content/src/project.rs)
- [Settings-store project parser at the verified commit](https://github.com/zed-industries/zed/blob/bebe92f469834a287f5a57ed78e8d51a918b8ada/crates/settings/src/settings_store.rs)
- [External agents](https://zed.dev/docs/ai/external-agents)
- [Model Context Protocol](https://zed.dev/docs/ai/mcp)
- [Edit predictions](https://zed.dev/docs/ai/edit-prediction)
- [Tasks](https://zed.dev/docs/tasks)
- [Debugger](https://zed.dev/docs/debugger)

The older dated migration documents remain historical evidence only. Do not
copy their version-specific snippets into current configuration.
