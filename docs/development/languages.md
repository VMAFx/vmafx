<!-- markdownlint-disable MD013 MD060 -->
# Languages used in VMAFX

VMAFX is a multi-language project. This page documents the role of each language,
the minimum toolchain versions required to build the full project, and pointers to
language-specific setup guides.

See [docs/principles.md §8](../principles.md#8-multi-language-policy-adr-0702) for
the policy constraints that govern which language is used for which role.

## C / C++23 — core library

**Used in:** `core/` (metric engine, feature extractors, GPU backend runtimes)

**Minimum version:** C23 (GCC ≥ 13 or Clang ≥ 17) / C++23 for new fork-added TUs.
Netflix-inherited C files remain C99-compatible and are migrated per-TU only when
a PR already touches the file.

**Required toolchain:**

```bash
# Linux — via package manager
sudo apt install gcc-13 clang-17   # or newer

# macOS — via Homebrew
brew install llvm
```

**Build:** see [build-flags.md](build-flags.md) for Meson options.

## Go — production tooling

**Used in:** `cmd/` (future: `cmd/vmafx-server`, `cmd/vmafx-mcp`, `cmd/vmafx-tune`)

**Minimum version:** Go 1.25 (go.mod: `go 1.26.4`; toolchain 1.26.4)

**Install:**

```bash
# Linux / macOS — via official installer
# https://go.dev/dl/
# or via mise / asdf / homebrew

brew install go          # macOS
sudo apt install golang  # Ubuntu (may not be 1.25 yet — prefer upstream)
```

**Verify:**

```bash
go version  # must be >= go1.25
```

**Workspace quick-start:**

```bash
go build ./...     # or: make go-build
go test ./...      # or: make go-test
go vet ./...       # static analysis (required CI gate)
```

The Go module root is `github.com/VMAFx/vmafx` (declared in `go.mod`).
Packages live under `pkg/`; binaries live under `cmd/`.

## Rust — FFI bindings + feature-extractor pilots

**Used in:** `bindings/rust/vmafx-sys` (FFI bindings crate),
`core/src/feature/rust/` (optional pilot feature extractors)

**Minimum version:** Rust stable (≥ 1.80 recommended; latest stable preferred)

**Install:**

```bash
# Install rustup (manages Rust toolchains)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
rustup update stable
```

**Verify:**

```bash
rustc --version   # must be stable
cargo --version
```

**Workspace quick-start:**

```bash
cargo check --all   # or: make rust-build
cargo test --all    # or: make rust-test
```

The Rust workspace manifest is at `Cargo.toml` in the repo root.
Members are added by per-sweep PRs (the foundation PR adds none).

## Python — ML training and dev scripts

**Used in:** `ai/` (PyTorch + Lightning), `tools/vmaf-tune/src/vmaftune/`,
`mcp-server/vmaf-mcp/`, `scripts/`

**Minimum version:** Python 3.11 (3.12 recommended)

**Setup:**

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -e ".[dev]"
```

See [dev-mcp.md](dev-mcp.md) for the full dev-container setup which pins
all Python dependencies in a stable environment.

## GPU compute — CUDA / SYCL / HIP / Metal / Vulkan GLSL

**Used in:** `core/src/feature/{cuda,sycl,hip}/` and `core/src/{cuda,sycl}/`

See the backend-specific guides:

- [docs/backends/cuda.md](../backends/cuda.md)
- [docs/backends/sycl.md](../backends/sycl.md)
- [docs/backends/hip.md](../backends/hip.md)
- [docs/backends/vulkan.md](../backends/vulkan.md)
- [docs/backends/metal.md](../backends/metal.md)

## CI toolchain matrix

| Language | CI gate | Workflow file |
|---|---|---|
| C / C++23 | clang-tidy, cppcheck, meson test | `.github/workflows/lint-and-format.yml` |
| Go | `go vet ./...` + `go test ./...` | `.github/workflows/go-ci.yml` |
| Rust | `cargo check --all` + `cargo test --all` | `.github/workflows/rust-ci.yml` |
| Python | ruff + mypy strict + pytest | `.github/workflows/python-ci.yml` |

## References

- [ADR-0702](../adr/0702-vmafx-phase4-language-modernization.md) — language
  modernization umbrella
- [ADR-0686](../adr/0686-vmafx-rebrand-aggressive-modernization.md) — parent rebrand
  umbrella
- [docs/principles.md §8](../principles.md#8-multi-language-policy-adr-0702) —
  policy constraints
