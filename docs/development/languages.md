<!-- markdownlint-disable MD013 MD060 -->
# Languages used in VMAFX

Use this page to find which language a part of the tree is written in, which
toolchain version it needs and which CI workflow checks it. VMAFX is a
multi-language project; each section below follows the same shape: where the
language is used, the minimum version, how to install it and how to verify it.

| Language | Used in | Minimum version | CI workflow |
|---|---|---|---|
| C / C++23 | `core/` | GCC 13 or Clang 17 | `lint-and-format.yml` |
| Go | `cmd/`, `pkg/` | `go` directive of `go.mod` | `go-ci.yml` |
| Rust | `bindings/rust/`, `core/src/feature/rust/` | stable | `rust-ci.yml` |
| Python | `ai/`, `tools/`, `mcp-server/`, `python/`, `scripts/` | per package, see [Python](#python-ml-training-and-dev-scripts) | `lint-and-format.yml`, `tests-and-quality-gates.yml` |
| GPU kernels | `core/src/feature/{cuda,sycl,hip,metal}/` | vendor SDK, see backend guides | `libvmaf-build-matrix.yml` |

See [docs/principles.md §8](../principles.md#8-multi-language-policy-adr-0702)
for
the policy constraints that govern which language is used for which role.

## C / C++23 — core library

**Used in:** `core/` (metric engine, feature extractors, GPU backend runtimes)

**Minimum version:** C23 (GCC ≥ 13 or Clang ≥ 17) / C++23 for new fork-added
TUs.
Netflix-inherited C files remain C99-compatible and are migrated per-TU only
when
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

**Used in:** `cmd/` (`vmafx-controller`, `vmafx-mcp`, `vmafx-node`,
`vmafx-operator`, `vmafx-ort-runner`, `vmafx-server`, `vmafx-tune`) and `pkg/`

**Required version:** the exact version declared by `go.mod` (currently Go
1.27.2). CI reads the same file through `actions/setup-go`.

**Install:**

```bash
# Linux / macOS — via official installer
# https://go.dev/dl/
# or via mise / asdf / homebrew

brew install go          # macOS
sudo apt install golang  # Ubuntu may lag go.mod — prefer the upstream installer
```

**Verify:**

```bash
go version  # must match the go directive in go.mod
```

**Workspace quick-start:**

```bash
meson setup core/build-cpu core && ninja -C core/build-cpu
make go-fix-check
make go-build
make go-test
```

The Go module root is `github.com/VMAFx/vmafx` (declared in `go.mod`).
Packages live under `pkg/`; binaries live under `cmd/`.

The Make targets explicitly link `pkg/libvmaf` against the in-tree fork.
For a direct Go invocation, select the same verified library; the package has
no implicit `-lvmaf` fallback because a missing build must not silently select
a distro or stale system copy:

```bash
export CGO_LDFLAGS="-L$(pwd)/core/build-cpu/src -lvmaf -lm"
export LD_LIBRARY_PATH="$(pwd)/core/build-cpu/src${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
go fix -diff ./...
go vet ./...
go test ./...
```

Go tests that score with the `vmaf` CLI use the build under test, never a host
install. `internal/vmaftest` resolves it from `VMAF_BIN` or, when that is unset,
`core/build-cpu/tools/vmaf` (the build the commands above link against); with
neither present the test fails and says how to build it. A `vmaf` on `PATH` or
under `/usr/local/bin` is not consulted, so a stale host binary cannot make a
test pass or fail for the wrong reason.

A Go test that runs a stub in place of a real tool (a fake `vmaf` that prints a
canned report, for example) writes it with `execstub.Write` from
`internal/execstub`, not with `os.WriteFile`. When parallel tests start
processes while a stub is being written, the forked child holds the stub's
write descriptor until it reaches its own exec, and Linux refuses to run a
file that is open for writing: the test fails with `text file busy`
([go.dev/issue/22315](https://go.dev/issue/22315)). `execstub.Write` holds
`syscall.ForkLock` while it writes the file, so no process starts in that
window. Writing to a temporary name and renaming does not help, because the
child's descriptor refers to the same file. `go test ./internal/execstub/`
runs 400 write-and-run cycles under concurrent forks and fails on any
`text file busy`.

`make go-fix` applies the pinned toolchain's source rewrites. Re-run it when
the tool asks for another pass, then use `make go-fix-check`; CI runs the same
non-mutating `go fix -diff ./...` check and rejects any remaining patch. The
default invocation covers the host Go package graph (CI: Linux/amd64 with
cgo); platform- or tag-exclusive files need a matching qualified invocation.

## Rust — FFI bindings + feature-extractor pilots

**Used in:** `bindings/rust/vmafx-sys` and `bindings/rust/vmafx` (FFI
bindings crates), `core/src/feature/rust/` (optional pilot feature extractors)

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

The Rust workspace manifest is at `Cargo.toml` in the repo root. Its members
are `bindings/rust/vmafx-sys`, `bindings/rust/vmafx` and
`core/src/feature/rust/tad`.

## Python: ML training and dev scripts

**Used in:** `ai/` (PyTorch + Lightning), `tools/vmaf-tune/src/vmaftune/`,
`mcp-server/vmaf-mcp/`, `scripts/`

**Minimum version:** each package declares its own floor in its
`pyproject.toml`:

| Package | `requires-python` |
|---|---|
| repository root (`pyproject.toml`, tool configuration only) | `>=3.14` |
| `ai/` | `>=3.11,<3.15` |
| `tools/vmaf-tune/` | `>=3.10,<3.15` |
| `dev-llm/` | `>=3.11` |
| `python/`, `mcp-server/vmaf-mcp/` | `>=3.10` |

CI installs Python 3.14 (`PYTHON_VERSION` in `build-config.env`).

**Setup:**

The repository root `pyproject.toml` contains only tool configuration
(Black, Ruff, Pytest, Mypy) for `vmafx-tooling`; it has no build system or
root project dependencies to install. Running `pip install -e .` fails
flat-layout package discovery. Instead, the repository uses per-package
editable installs for its independent distributions. See
[python-test-orchestrator.md](python-test-orchestrator.md) for the `nox`
per-package virtual environments.

The canonical environment is the dev container (hard rule 12 of
[agent-hard-rules.md](agent-hard-rules.md)); the `python-env` stage of
[`dev/Containerfile`](../../dev/Containerfile) is the authoritative install
list. On the host, the verified recipe is:

```bash
python3 -m venv .venv
.venv/bin/pip install --upgrade pip
.venv/bin/pip install "meson==1.12.1" ninja pre-commit pytest nox
.venv/bin/pip install -r python/requirements.txt
.venv/bin/pip install -e python
.venv/bin/pip install -e mcp-server/vmaf-mcp
.venv/bin/pip install -e "tools/vmaf-tune[fast]"
.venv/bin/pip install -e dev-llm
# optional, heavy (PyTorch): .venv/bin/pip install -e "ai[dev]"
```

Pin Meson to the version in `requirements/locks/build.in` (currently
`meson==1.12.1`). Meson build directories record the absolute path of the
generator binary, so mismatching Meson executables break `ninja`
re-generation.

See [dev-mcp.md](dev-mcp.md) for the full dev-container setup which pins
all Python dependencies in a stable environment.

## GPU compute — CUDA / SYCL / HIP / Metal

**Used in:** `core/src/feature/{cuda,sycl,hip,metal}/` and
`core/src/{cuda,sycl,hip,metal}/`

See the backend-specific guides:

- [CUDA](../backends/cuda/overview.md)
- [SYCL](../backends/sycl/overview.md)
- [HIP](../backends/hip/overview.md)
- [Vulkan removal notice](../backends/vulkan/overview.md)
- [Metal](../backends/metal/index.md)

## CI toolchain matrix

| Language | CI gate | Workflow file |
|---|---|---|
| C / C++23 | clang-tidy, cppcheck, credential-safe Meson test runner | `.github/workflows/lint-and-format.yml` |
| Go | `go fix -diff ./...` + `go vet ./...` + `go test ./...` | `.github/workflows/go-ci.yml` |
| Rust | `cargo fmt --all` + `cargo clippy --workspace` + `cargo test --workspace --all-features` | `.github/workflows/rust-ci.yml` |
| Python | ruff (pre-commit hooks) + mypy delta gate + pytest | `.github/workflows/lint-and-format.yml` (Python Lint), `.github/workflows/tests-and-quality-gates.yml` (pytest; which job runs which suite: [test suites](test-suites.md)) |

## References

- [ADR-0702](../adr/0702-vmafx-phase4-language-modernization.md) — language
  modernization umbrella
- [ADR-0686](../adr/0686-vmafx-rebrand-aggressive-modernization.md) — parent
  rebrand
  umbrella
- [docs/principles.md §8](../principles.md#8-multi-language-policy-adr-0702) —
  policy constraints

## History

### Recovering a destroyed venv (legacy symlink bug)

If `.venv` fails with `env: 'bash': Too many levels of symbolic links` or a
`.venv -> .venv` self-loop, a legacy tracked `.venv` symlink (fixed in PR #1280)
clobbered the environment. Remove the broken path (`rm -rf .venv`) and rerun
the setup recipe above to recreate a clean virtualenv.
