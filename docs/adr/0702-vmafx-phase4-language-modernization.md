<!-- markdownlint-disable MD013 MD060 -->
# ADR-0702: VMAFX Phase 4 — Multi-Language Modernization Foundation

- **Status**: Proposed
- **Date**: 2026-05-28
- **Deciders**: Lusoris
- **Tags**: go, rust, cpp23, language-policy, modernization, tooling, fork-local, phase4

## Context

The VMAFX rebrand umbrella (ADR-0686) established an aggressive modernization
agenda: C23 for the core library, C++23 for select internals, and new production
tooling. ADR-0686 deferred the specific language migration plan to a follow-up
per-sweep ADR; this ADR is that follow-up.

The fork currently hosts the following language surfaces:

- **C** (core) — `core/` metric engine, feature extractors, GPU backends
- **Python** — `ai/` ML training, `tools/vmaf-tune/`, `mcp-server/`, `scripts/`
- **CUDA / SYCL / HIP / Metal / Vulkan GLSL** — GPU compute kernels

Three forces motivate adding Go and Rust:

1. **Production tooling complexity.** The vmaf-tune, vmafx-server, and vmafx-mcp
   components have grown beyond what the Python harness was designed for. A compiled,
   statically-typed language with first-class concurrency and a small binary footprint
   is a better fit for these server and CLI roles. Go was chosen over alternatives
   (Rust, Zig) for its lower ramp-up cost, excellent standard-library HTTP/gRPC story,
   and straightforward FFI via cgo.

2. **Safe FFI and near-zero-overhead bindings.** Rust's memory-safety model, strong
   type system, and `bindgen`-based `unsafe` FFI layer provide the best story for
   shipping libvmaf bindings that will be consumed by downstream Rust crates. A pilot
   `vmafx-sys` crate establishes the pattern for the binding layer and provides a
   reference for the Rust community to build on.

3. **Incremental C++-23 migration.** ADR-0692 bumped the C standard target to C23.
   The next natural step is to adopt C++23 for modules and subsystems where C++'s
   abstractions (RAII, constexpr, std::span, std::expected) reduce the chance of
   resource-management bugs without adding overhead. The migration is incremental —
   per-TU, triggered only when a PR touches the file — to avoid a disruptive full-tree
   churn.

The foundation PR (this ADR) establishes the workspace skeletons and toolchain wiring
so that subsequent per-sweep PRs can land independently without colliding on
project-level manifests.

## Decision

### Go workspace

- A `go.mod` workspace manifest is added at the repo root with module name
  `github.com/VMAFx/vmafx` and minimum Go 1.23.
- Directories `cmd/` and `pkg/` are created as placeholders for future CLI binaries
  and shared library packages.
- A single Go file `pkg/version/version.go` exports a `Version() string` function
  as the minimal "workspace compiles" smoke test.
- A `go-ci.yml` GitHub Actions workflow runs `go vet ./...` and `go test ./...` on
  any PR that touches `**/*.go` files.
- `Makefile` gains `go-build` and `go-test` targets.

Go is chosen for production tooling (`cmd/vmafx-server`, `cmd/vmafx-mcp`,
`cmd/vmafx-tune`). Subsequent per-sweep PRs add those binaries.

### Rust workspace

- A `Cargo.toml` workspace manifest is added at the repo root with members pointing
  to `bindings/rust/vmafx-sys`.
- Directories `bindings/rust/` and `core/src/feature/rust/` are created as
  placeholders for the FFI crate and any future Rust feature extractors.
- A `rust-ci.yml` GitHub Actions workflow runs `cargo check --all` and
  `cargo test --all` on any PR that touches `**/*.rs` or `Cargo.toml` files.
- `Makefile` gains `rust-build` and `rust-test` targets.

Rust is scoped to two roles: libvmaf FFI bindings (`bindings/rust/vmafx-sys`) and an
optional pilot feature extractor under `core/src/feature/rust/`. These will be filled
by dedicated follow-up PRs.

### C++23 internals migration

No code is converted in this PR. The policy is recorded here for subsequent PRs:

- New fork-added `.cpp` files use `-std=c++23`.
- Existing C files are migrated per-TU only when a PR already touches the file for
  another reason.
- The C ABI at `core/include/libvmaf/` is permanently preserved; C++ internals sit
  entirely behind that boundary.
- `.clang-tidy` will be extended with a C++23 module check in a follow-up ADR.

### Python is unchanged

`ai/` (PyTorch + Lightning), `tools/vmaf-tune/src/vmaftune/`, `mcp-server/`,
and `scripts/` remain Python. No Python → Go rewrite is planned or implied.

### Per-sweep child ADRs (planned)

The following ADRs will follow in separate PRs:

| Child ADR slug | Scope |
|---|---|
| `vmafx-go-server` | `cmd/vmafx-server` HTTP + gRPC production binary |
| `vmafx-go-mcp` | `cmd/vmafx-mcp` replacement for the Python MCP server |
| `vmafx-go-tune` | `cmd/vmafx-tune` replacement for the Python vmaf-tune CLI |
| `vmafx-rust-sys` | `bindings/rust/vmafx-sys` FFI bindings crate |
| `vmafx-rust-feature-extractor-pilot` | First Rust feature extractor under `core/src/feature/rust/` |
| `vmafx-cpp23-migration-policy` | Per-TU C++23 adoption gates and clang-tidy extensions |

Each child ADR supersedes a portion of this umbrella and fills in implementation
details this foundation PR cannot predict.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep Python for all tooling | Zero new toolchains; existing contributors know Python | Concurrency model, binary size, and startup latency are poor fits for a production scoring server; type safety requires external tooling (mypy) that lags the runtime | The user directed a Go rewrite for tooling; Python stays only for ML training and dev scripts |
| Use Rust for all tooling (no Go) | Single compiled language; maximum safety | Steeper learning curve; std HTTP/gRPC story requires more third-party crates; go-based infrastructure tooling in the k8s/CI/CD ecosystem is richer | The user explicitly chose both: Go for tooling, Rust for bindings pilot |
| Use Zig for systems work | Excellent C interop; comptime is powerful | Very small ecosystem; no stable ABI guarantee; fewer contributors available | Rejected; not mentioned by the user and ramp-up cost exceeds benefit |
| Single combined `Cargo.toml` + `go.work` super-workspace | One command builds everything | Go workspaces and Cargo workspaces are not composable at the repo root without custom harness; adds confusion for single-language contributors | Rejected; keep them as parallel peer manifests at the repo root |
| Defer workspace skeletons to per-sweep PRs | Smaller foundation PR | Each per-sweep PR would collide on `go.mod` / `Cargo.toml` creation if they land in parallel | Rejected; the foundation PR exists precisely to prevent that collision |

## Consequences

### Positive

- Parallel Go and Rust per-sweep agents can begin immediately after this PR merges
  without colliding on workspace manifests.
- `go vet` and `cargo check` gates catch compilation errors on the very first Go or
  Rust file before the module grows large.
- The language policy is explicit and auditable; new contributors know immediately
  which language to use for which role.

### Negative

- Two new toolchains (Go ≥ 1.23, Rust stable) are required to build the full repo.
  Contributors who only work on the C/Python surface do not need them, but CI must
  install both.
- `go.mod` at the repo root is a named Go module (`github.com/VMAFx/vmafx`) — any
  future module rename is a breaking change for downstream Go consumers.

### Neutral / follow-ups

- Each per-sweep child ADR listed in the Decision section will supersede the
  corresponding placeholder section of this ADR when it lands.
- The `Cargo.lock` policy for workspace-level binaries (keep under VCS) versus
  library crates (gitignore) is established in the root `.gitignore`; the
  `vmafx-sys` crate is a library crate and its `Cargo.lock` is gitignored.

## References

- [ADR-0686](0686-vmafx-rebrand-aggressive-modernization.md) — parent umbrella ADR
  (VMAFX rebrand); this ADR is a direct child.
- [ADR-0692](0692-vmafx-c23-bump.md) — C23 standard target bump.
- [ADR-0701](0701-vmafx-cloud-native-redesign.md) — cloud-native redesign (HTTP/gRPC
  server model that the Go binaries will implement).
- [ADR-0700](0700-vmafx-repo-layout.md) — repo layout (`core/`, `compat/python-vmaf/`).

User direction (verbatim, per CLAUDE.md References rule):

- `"number 2 but test rust as well"` — user's choice for tooling language (option 2
  was Go; addendum was to add a Rust pilot alongside Go).
- `"Both: C++23 internals + Rust pilot"` — user's answer when asked about core
  language strategy.
