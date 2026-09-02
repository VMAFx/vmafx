<!-- markdownlint-disable MD013 MD060 -->
# ADR-0712: IDE config audit and refresh for multi-language post-rebrand VMAFX

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: docs, ide, go, rust, build, phase4, vmafx

## Context

Prior to the VMAFX rebrand (ADR-0700 through ADR-0709), the IDE configs
(`.vscode/` and `.zed/`) were calibrated for a pure C/C++ project rooted at
`libvmaf/`. The rebrand and Phase 4 language-modernisation work (ADR-0702,
ADR-0703, ADR-0704, ADR-0705, ADR-0706) added four new first-class languages
to the repo:

- **Go 1.23** — `cmd/vmafx-server`, `cmd/vmafx-mcp`, `cmd/vmafx-tune`, `pkg/`
- **Rust** — `bindings/rust/vmafx-sys/`, `core/src/feature/rust/` (ADR-0706)
- **Protobuf / gRPC** — `proto/vmafx.proto`, generated code in `gen/`
- **Helm / Kubernetes YAML** — `deploy/helm/vmafx/` (ADR-0699)

Additionally, the `libvmaf/` → `core/` directory rename (ADR-0700) left several
paths in `c_cpp_properties.json` and the clangd `--compile-commands-dir` pointing
at non-existent locations.

Stale state in the existing configs:

1. `c_cpp_properties.json` — `configuration.name = "libvmaf"`, include paths
   reference `${workspaceFolder}/libvmaf/include` and `${workspaceFolder}/libvmaf/src`.
2. `settings.json` clangd fallbackFlags — `-I${workspaceFolder}/core/include` and
   `-I${workspaceFolder}/core/src` (already updated during ADR-0700 sweep, correct).
   But `--compile-commands-dir` still points at `${workspaceFolder}/build` which is
   a generic name; no Go/Rust/Proto LSP configuration is present.
3. `tasks.json` — meson setup commands still reference `libvmaf` as the source
   directory argument.
4. `extensions.json` — missing `golang.go`, `rust-lang.rust-analyzer`,
   `zxh404.vscode-proto3`, `ms-kubernetes-tools.vscode-kubernetes-tools`,
   `ms-azuretools.vscode-docker`, `bierner.markdown-mermaid`,
   `vivaxy.vscode-conventional-commits`.
5. `launch.json` — binary paths reference `build/tools/vmaf`; correct but not
   updated to reflect post-rebrand tool name `vmafx`.
6. `.zed/settings.json` — clangd fallbackFlags still reference `./libvmaf/include`
   and `./libvmaf/src`; no Go/Rust LSP entries.

The decision is **non-trivial** (another engineer could have chosen to delay IDE
config updates until the new language surfaces are more mature) because developer
experience directly affects contributor onboarding velocity: missing LSP entries
for Go and Rust mean contributors open the repo and immediately see red squiggles,
inferring that the build is broken rather than that the IDE config predates the
new surfaces.

## Decision

Update `.vscode/extensions.json`, `.vscode/settings.json`,
`.vscode/c_cpp_properties.json`, `.vscode/tasks.json`, `.vscode/launch.json`,
and `.zed/settings.json` in a single PR:

1. **Remove** `twxs.cmake` from recommendations (cmake-tools has been in
   `unwantedRecommendations` since the meson migration; the cmake syntax extension
   is similarly dead-weight).
2. **Add** seven extensions to `extensions.json` recommendations:
   `golang.go`, `rust-lang.rust-analyzer`, `zxh404.vscode-proto3`,
   `ms-kubernetes-tools.vscode-kubernetes-tools`, `ms-azuretools.vscode-docker`,
   `bierner.markdown-mermaid`, `vivaxy.vscode-conventional-commits`.
3. **Fix `c_cpp_properties.json`** — rename the configuration from `"libvmaf"` to
   `"core"`, update `compileCommands` to `core/build-cpu/compile_commands.json`,
   and update include paths to `core/include` and `core/src`.
4. **Update `settings.json`** — add Go, Rust, and proto language settings; add
   file associations for `.proto`, `.tmpl`, `.hip`, `.metal`; add Go/Rust build
   directories to `watcherExclude` and `search.exclude`; add `go.lintTool`,
   `rust-analyzer.cargo.allTargets`, and `[go]`/`[rust]`/`[proto3]` formatter blocks.
5. **Update `tasks.json`** — remove the `libvmaf` argument from meson setup
   commands; add Go build/test tasks; add Rust build/test tasks.
6. **Update `launch.json`** — rename debug configurations to reference `vmafx`
   tool name at `build/tools/vmafx`.
7. **Update `.zed/settings.json`** — fix clangd fallbackFlags to reference
   `./core/include` and `./core/src`; add gopls and rust-analyzer LSP entries;
   add Go/Rust language formatter blocks; update `context_servers` MCP binary
   path; add `.proto` and `.tmpl` file-type associations.
8. **Update `docs/development/ide-setup.md`** — rewrite to document the
   multi-language setup including Go, Rust, proto, Helm per-language LSP notes.

## Alternatives considered

| Alternative | Reason not chosen |
|---|---|
| Defer until Phase 4b surfaces are more stable | Contributor UX degrades immediately; Go and Rust surfaces are already merged and shipping CI. The cost of updating IDE configs is low (~2 h). |
| Only update C/C++ paths; skip Go/Rust | Half-measures cause confusion — contributors opening `cmd/vmafx-server/` see missing gopls while `cmd/` claims to be production code. |
| Replace `.vscode/` entirely with a devcontainer.json config | Large scope change; devcontainer is a separate Phase 4b effort (ADR-0709). IDE configs and devcontainer coexist. |

## Consequences

**Positive:**

- Contributors opening the repo in VS Code or Zed immediately get working
  LSP for all four languages without manual configuration.
- `c_cpp_properties.json` no longer references the deleted `libvmaf/` path,
  eliminating the "configuration not found" warning in clangd fallback mode.
- Go and Rust build output directories excluded from file watcher and search,
  preventing IDE slowdowns as those trees grow.

**Negative:**

- Developers who had customised their local `.vscode/settings.json` via
  workspace-local overrides will not be affected (workspace overrides are not
  committed), but contributors who rely on the project-level settings.json
  directly will observe formatter and LSP changes on next VS Code reload.

**Neutral follow-ups:**

- When `cmd/vmafx-controller` and `cmd/vmafx-node` are scaffolded (ADR-0709
  Phase 4b), no additional IDE config changes are required — gopls discovers
  `go.mod` at the root automatically.
- The `twxs.cmake` removal is a recommendation change only; contributors who
  have it installed are unaffected.

## References

- ADR-0700 (repo layout, `libvmaf/` → `core/`)
- ADR-0702 (Phase 4 multi-language modernization foundation)
- ADR-0703 (vmafx-server Go/gRPC)
- ADR-0704 (vmafx-mcp Go port)
- ADR-0705 (vmafx-tune Go Stage 1)
- ADR-0706 (vmafx-sys Rust bindings)
- ADR-0709 (Phase 4b distributed platform umbrella)
- `req`: user requested IDE audit after rebrand changes (paraphrased: "after all the changes we are now doing we for sure need to recheck the vscode settings (especially plugins)")
