- VS Code and Zed IDE configs refreshed for the multi-language VMAFX post-rebrand stack:
  added `golang.go`, `rust-lang.rust-analyzer`, `zxh404.vscode-proto3`,
  `ms-kubernetes-tools.vscode-kubernetes-tools`, `ms-azuretools.vscode-docker`,
  `bierner.markdown-mermaid`, and `vivaxy.vscode-conventional-commits` extension
  recommendations; removed the stale `twxs.cmake` entry; fixed `c_cpp_properties.json`
  to reference `core/` (was `libvmaf/`); updated clangd fallbackFlags in both
  `.vscode/settings.json` and `.zed/settings.json`; added Go/Rust LSP (gopls,
  rust-analyzer), formatter, and task entries; added file associations for `.proto`,
  `.tmpl`, `.hip`, `.metal`; extended watcher/search excludes to cover Go `target/`
  and per-backend `build-{cpu,cuda,sycl,all}/` dirs; updated all `vmaf` binary
  references to `vmafx` (ADR-0712). See
  [docs/development/ide-setup.md](docs/development/ide-setup.md).
